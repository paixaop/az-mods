/* SPDX-License-Identifier: GPL-3.0-only */
#include "cues.h"
#include "azmod-timestamps.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Raw PSSI mood/kind mapping follows Anchor. SDK category=0 deliberately does
 * not promise normalized labels. Unknown kinds still match the Any selector. */
static int phrase_category(const az_phrase *p)
{
    if (p->raw_mood == 1) {
        switch (p->raw_kind) {
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 5: return 4;
        case 6: return 5;
        default: return 0;
        }
    }
    if (p->raw_mood == 2 || p->raw_mood == 3) {
        switch (p->raw_kind) {
        case 1: return 1;
        case 2: case 3: case 4: case 5: case 6: case 7: return 6;
        case 8: return 7;
        case 9: return 4;
        case 10: return 5;
        default: return 0;
        }
    }
    /* No mood means no trustworthy classification. */
    return 0;
}

static int terminated(const char *s, size_t n) { return memchr(s, 0, n) != NULL; }
static int fold(unsigned char c) { return c < 128 ? tolower(c) : c; }
static int same_text(const char *a, const char *b)
{
    while (*a && *b && fold((unsigned char)*a) == fold((unsigned char)*b)) { ++a; ++b; }
    return !*a && !*b;
}
static int contains_text(const char *haystack, const char *needle)
{
    if (!*needle) return 1;
    for (; *haystack; ++haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n && fold((unsigned char)*h) == fold((unsigned char)*n)) { ++h; ++n; }
        if (!*n) return 1;
    }
    return 0;
}
static int ready(const az_component_info *info) { return info->availability == AZ_AVAIL_READY; }

static int group_matches(const cues_config *c, const cues_group *g, const az_track_snapshot *s)
{
    const az_track_metadata *m = &s->metadata;
    if (!g->enabled || !terminated(g->artist, sizeof g->artist) || !terminated(g->key, sizeof g->key)) return 0;
    if (*g->artist && (!(m->valid & AZ_META_ARTIST) || !terminated(m->artist, sizeof m->artist) || !contains_text(m->artist, g->artist))) return 0;
    if (*g->key && (!(m->valid & AZ_META_KEY) || !terminated(m->key, sizeof m->key) || !same_text(m->key, g->key))) return 0;
    double low = g->min_bpm > 0 ? g->min_bpm : c->min_bpm;
    double high = g->max_bpm > 0 ? g->max_bpm : c->max_bpm;
    if (low > 0 || high > 0) {
        if (!(m->valid & AZ_META_TEMPO) || !isfinite(m->original_bpm) || m->original_bpm <= 0) return 0;
        if ((low > 0 && m->original_bpm < low) || (high > 0 && m->original_bpm > high)) return 0;
    }
    return 1;
}

typedef struct {
    const az_track_snapshot *track;
    uint32_t rate;
    int grid_valid, duration_valid;
    uint32_t bar_count;
    uint16_t bars[32768]; /* bounded worker scratch; grid rows never exceed this */
} clock_map;

/* Grid coordinates come only from measured boundaries. In particular, the
 * track's duration is not a substitute for an unobserved final grid beat. */
static int grid_valid(const az_track_snapshot *s, uint32_t rate)
{
    if (!ready(&s->grid_info) || !s->beats || s->beat_count < 2 || s->beat_count > 32768) return 0;
    for (uint32_t i = 0; i < s->beat_count; ++i) {
        const az_beat *b = &s->beats[i];
        int nonnegative = compare_timestamps(b->time_s, 0., 1, rate, 1);
        if (nonnegative == AZ_TIME_INVALID || nonnegative == AZ_TIME_BEFORE) return 0;
        if (i) {
            int order = compare_timestamps(b->time_s, s->beats[i - 1].time_s, 1, rate, 1);
            if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER) return 0;
        }
    }
    return 1;
}
static int time_range(const clock_map *m, double t)
{
    int zero = compare_timestamps(t, 0., 1, m->rate, 1);
    if (zero == AZ_TIME_INVALID || zero == AZ_TIME_BEFORE || !m->duration_valid) return 0;
    int end = compare_timestamps(t, m->track->metadata.duration_s, 1, m->rate, 1);
    if (end == AZ_TIME_INVALID) return 0;
    return end == AZ_TIME_BEFORE;
}
static int time_at(const clock_map *m, int unit, double coordinate, double *time)
{
    if (!isfinite(coordinate)) return 0;
    if (unit == CUES_SECONDS) { *time = coordinate; return isfinite(*time); }
    if (!m->grid_valid || coordinate < 0) return 0;
    const az_track_snapshot *s = m->track;
    double whole = floor(coordinate), fraction = coordinate - whole;
    if (whole > s->beat_count) return 0;
    uint32_t index = (uint32_t)whole;
    if (unit == CUES_BEATS) {
        if (index >= s->beat_count) return 0;
        if (fraction > 0) {
            if (index + 1 >= s->beat_count) return 0;
            *time = s->beats[index].time_s + fraction * (s->beats[index + 1].time_s - s->beats[index].time_s);
        } else *time = s->beats[index].time_s;
        return isfinite(*time);
    }
    if (unit != CUES_BARS) return 0;
    if (index >= m->bar_count) return 0;
    uint32_t first = m->bars[index];
    if (!(fraction > 0)) { *time = s->beats[first].time_s; return 1; }
    if (index + 1 >= m->bar_count) return 0;
    uint32_t next = m->bars[index + 1];
    *time = s->beats[first].time_s + fraction * (s->beats[next].time_s - s->beats[first].time_s);
    return isfinite(*time);
}
static int coordinate_at(const clock_map *m, int unit, double time, double *coordinate)
{
    if (!isfinite(time)) return 0;
    if (unit == CUES_SECONDS) { *coordinate = time; return 1; }
    if (!m->grid_valid || (unit != CUES_BEATS && unit != CUES_BARS)) return 0;
    const az_track_snapshot *s = m->track;
    uint32_t low = 0, high = unit == CUES_BARS ? m->bar_count : s->beat_count;
    uint32_t count = high;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint32_t index = unit == CUES_BARS ? m->bars[middle] : middle;
        int order = compare_timestamps(time, s->beats[index].time_s, 1, m->rate, 1);
        if (order == AZ_TIME_INVALID) return 0;
        if (order == AZ_TIME_EQUAL) { *coordinate = middle; return 1; }
        if (order == AZ_TIME_BEFORE) high = middle;
        else low = middle + 1;
    }
    if (!low || low == count) return 0;
    uint32_t previous = unit == CUES_BARS ? m->bars[low - 1] : low - 1;
    uint32_t next = unit == CUES_BARS ? m->bars[low] : low;
    *coordinate = low - 1 + (time - s->beats[previous].time_s) / (s->beats[next].time_s - s->beats[previous].time_s);
    return isfinite(*coordinate);
}
static int shift(const clock_map *m, double base, double amount, int unit, double *out)
{
    double coordinate;
    return coordinate_at(m, unit, base, &coordinate) && time_at(m, unit, coordinate + amount, out);
}
static int quantize(const clock_map *m, int mode, double input, double *out)
{
    if (mode == CUES_QUANT_OFF) { *out = input; return isfinite(input); }
    if (!m->grid_valid || (mode != CUES_QUANT_BEAT && mode != CUES_QUANT_BAR)) return 0;
    int unit = mode == CUES_QUANT_BAR ? CUES_BARS : CUES_BEATS;
    double coordinate, lo, hi;
    if (!coordinate_at(m, unit, input, &coordinate) || !time_at(m, unit, floor(coordinate), &lo)) return 0;
    if (!time_at(m, unit, ceil(coordinate), &hi)) return 0;
    /* Nearest in source time; an exact tie goes to the earlier boundary. */
    int near = compare_timestamps(input - lo, hi - input, 1, m->rate, 1);
    if (near == AZ_TIME_INVALID) return 0;
    *out = near == AZ_TIME_AFTER ? hi : lo;
    return 1;
}

/* Copy complete UTF-8 codepoints only; strings were validated at import. */
static size_t utf8_width(const char *s)
{
    unsigned char c = (unsigned char)*s;
    size_t n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!n) return 0;
    for (size_t i = 1; i < n; ++i) if (((unsigned char)s[i] & 0xc0) != 0x80) return 0;
    return n;
}
static void append(char *dst, size_t capacity, const char *src)
{
    size_t used = strlen(dst);
    while (*src) {
        size_t n = utf8_width(src);
        if (!n || used + n >= capacity) break;
        memcpy(dst + used, src, n);
        used += n;
        src += n;
    }
    dst[used] = 0;
}
static void expand(char *out, size_t size, const char *pattern, const char *phrase, const char *length)
{
    out[0] = 0;
    while (*pattern && strlen(out) + 1 < size) {
        if (!strncmp(pattern, "{phrase}", 8)) { append(out, size, phrase); pattern += 8; }
        else if (!strncmp(pattern, "{length}", 8)) { append(out, size, length); pattern += 8; }
        else {
            size_t n = utf8_width(pattern);
            if (!n) break;
            char one[5] = {0};
            memcpy(one, pattern, n);
            if (strlen(out) + n >= size) break;
            append(out, size, one);
            pattern += n;
        }
    }
}
static void label(cues_point *point, const cues_rule *r, const cues_group *g,
                  const clock_map *m, int category, double phrase_start, double phrase_end)
{
    char name[128], comment[128], length[32] = "0";
    double start = r->anchor == CUES_PHRASE ? phrase_start : point->start_s;
    double end = r->anchor == CUES_PHRASE ? phrase_end : point->end_s;
    if (r->anchor == CUES_PHRASE || point->type == AZ_CUE_LOOP) {
        double a, b;
        if (coordinate_at(m, g->measures_in, start, &a) && coordinate_at(m, g->measures_in, end, &b))
            snprintf(length, sizeof length, "%.6g", b - a);
        else snprintf(length, sizeof length, "?");
    }
    const char *phrase = r->anchor == CUES_PHRASE ? cues_phrase_names[category] : "Absolute";
    expand(name, sizeof name, r->name, phrase, length);
    expand(comment, sizeof comment, r->comment, phrase, length);
    point->comment[0] = 0;
    append(point->comment, sizeof point->comment, name);
    if (*name && *comment) append(point->comment, sizeof point->comment, ": ");
    append(point->comment, sizeof point->comment, comment);
}

static int near_point(double first, double second, double distance, uint32_t rate)
{
    int equal = compare_timestamps(first, second, 1, rate, 1);
    if (equal == AZ_TIME_INVALID || equal == AZ_TIME_EQUAL) return 1;
    if (!(distance > 0)) return 0;
    int order = compare_timestamps(fabs(first - second), distance, 1, rate, 1);
    return order == AZ_TIME_INVALID || order == AZ_TIME_BEFORE;
}
/* Check immutable occupancy before evaluating more phrase candidates. This
 * bounds the costly coordinate work to slots that can actually be filled. */
static int room(const cues_config *c, const az_native_cue_snapshot *existing,
                const cues_plan *out, int classification, int slot)
{
    int memory = 0;
    if (out->count >= CUES_MAX_PLAN) return 0;
    if (classification == AZ_CUE_HOT && (slot < 1 || slot > c->hot_pads)) return 0;
    for (uint32_t i = 0; i < existing->count; ++i) {
        const az_cue_entry *p = &existing->entries[i];
        memory += p->classification == AZ_CUE_MEMORY;
        if (classification == AZ_CUE_HOT && p->classification == AZ_CUE_HOT && p->slot == (unsigned)slot) return 0;
    }
    for (int i = 0; i < out->count; ++i) {
        const cues_point *p = &out->points[i];
        memory += p->classification == AZ_CUE_MEMORY;
        if (classification == AZ_CUE_HOT && p->classification == AZ_CUE_HOT && p->slot == (unsigned)slot) return 0;
    }
    return classification == AZ_CUE_MEMORY ? memory < c->max_memory && memory < 10 : 1;
}
static int insert(const cues_config *c, const az_native_cue_snapshot *existing,
                  uint32_t rate, cues_plan *out, const cues_point *candidate)
{
    int memory = 0, hot = 0;
    for (uint32_t i = 0; i < existing->count; ++i) {
        const az_cue_entry *p = &existing->entries[i];
        if (p->classification == AZ_CUE_MEMORY) ++memory;
        if (p->classification == AZ_CUE_HOT) ++hot;
        if (candidate->classification == AZ_CUE_HOT && p->classification == AZ_CUE_HOT && p->slot == candidate->slot) return 0;
        if ((p->classification == AZ_CUE_HOT || p->classification == AZ_CUE_MEMORY) &&
            (p->valid & AZ_CUE_START) && near_point(p->start_s, candidate->start_s, c->min_distance, rate)) return 0;
    }
    for (int i = 0; i < out->count; ++i) {
        const cues_point *p = &out->points[i];
        if (p->classification == AZ_CUE_MEMORY) ++memory;
        if (p->classification == AZ_CUE_HOT) ++hot;
        if (candidate->classification == AZ_CUE_HOT && p->classification == AZ_CUE_HOT && p->slot == candidate->slot) return 0;
        if (near_point(p->start_s, candidate->start_s, c->min_distance, rate)) return 0;
    }
    if (candidate->classification == AZ_CUE_MEMORY ? memory >= c->max_memory || memory >= 10
                                                 : hot >= 8 || candidate->slot > (unsigned)c->hot_pads) return 0;
    if (out->count >= CUES_MAX_PLAN) return 0;
    out->points[out->count++] = *candidate;
    return 1;
}
static int make_point(const cues_config *c, const cues_group *g, const cues_rule *r,
                      const clock_map *m, const az_native_cue_snapshot *existing,
                      cues_plan *out, int category, double start, double end)
{
    cues_point p = {0};
    double anchor = 0;
    if (r->anchor == CUES_ABSOLUTE) {
        if (!time_at(m, r->unit, r->position, &anchor)) return 0;
    } else if (r->phrase_point == CUES_START) anchor = start;
    else if (r->phrase_point == CUES_MIDDLE) anchor = (start + end) / 2;
    else anchor = end;
    /* An explicit zero measurement is the identity operation, including when
     * the provider has no bar model. Nonzero bar offsets still require it. */
    if (r->anchor == CUES_PHRASE && r->offset && !shift(m, anchor, r->offset, r->offset_unit, &anchor)) return 0;
    int quant = g->quantization == CUES_INHERIT ? c->quantization : g->quantization;
    if (!quantize(m, quant, anchor, &p.start_s)) return 0;
    p.type = AZ_CUE_POINT;
    if (r->loop_mode != CUES_NO_LOOP) {
        p.type = AZ_CUE_LOOP;
        if (r->loop_mode == CUES_LOOP_FIXED) {
            if (!(r->loop_length > 0) || !shift(m, p.start_s, r->loop_length, r->loop_unit, &p.end_s)) return 0;
        } else if (r->anchor != CUES_PHRASE) return 0;
        else if (r->loop_mode == CUES_LOOP_FROM_START) {
            p.end_s = p.start_s;
            if (!quantize(m, quant, start, &p.start_s)) return 0;
        } else if (r->loop_mode == CUES_LOOP_TO_END) {
            if (!quantize(m, quant, end, &p.end_s)) return 0;
        } else return 0;
        int order = compare_timestamps(p.end_s, p.start_s, 1, m->rate, 1);
        if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER ||
            !time_range(m, p.end_s)) return 0;
    }
    if (!time_range(m, p.start_s)) return 0;
    p.classification = (uint32_t)r->cue_type;
    p.slot = r->cue_type == AZ_CUE_HOT ? (uint32_t)r->pad : 0;
    p.rgb = cues_colors[r->color];
    label(&p, r, g, m, category, start, end);
    return insert(c, existing, m->rate, out, &p);
}

int cues_build_plan(const cues_config *c, const az_track_snapshot *s,
                    const az_native_cue_snapshot *existing, uint32_t sample_rate, cues_plan *out)
{
    if (!out) return AZ_INVALID;
    memset(out, 0, sizeof *out);
    if (!c || !s || !existing || !sample_rate || sample_rate >= 0x7fffffffu ||
        s->size < sizeof *s || s->version != AZ_DATA_VERSION ||
        existing->size < sizeof *existing || existing->version != AZ_DATA_VERSION ||
        !existing->revision || !existing->native_load_generation || existing->count > AZ_NATIVE_CUE_LIMIT ||
        c->group_count < 0 || c->group_count > CUES_MAX_GROUPS || c->max_memory < 0 || c->max_memory > 10 ||
        c->hot_pads < 1 || c->hot_pads > 8 || c->quantization < CUES_QUANT_OFF || c->quantization > CUES_QUANT_BAR ||
        c->measures_in < CUES_BARS || c->measures_in > CUES_SECONDS ||
        !isfinite(c->min_distance) || c->min_distance < 0 || !isfinite(c->min_bpm) || !isfinite(c->max_bpm)) return AZ_INVALID;
    if (!c->enabled) return AZ_OK;
    if (!ready(&s->metadata_info)) return AZ_PENDING;
    clock_map map = {.track = s, .rate = sample_rate};
    map.grid_valid = grid_valid(s, sample_rate);
    if (map.grid_valid) for (uint32_t i = 0; i < s->beat_count; ++i)
        if (s->beats[i].beat_in_bar == 1) map.bars[map.bar_count++] = (uint16_t)i;
    int duration_order = compare_timestamps(s->metadata.duration_s, 0., 1, sample_rate, 1);
    map.duration_valid = (s->metadata.valid & AZ_META_DURATION) &&
        duration_order != AZ_TIME_INVALID && duration_order == AZ_TIME_AFTER;
    if (!map.duration_valid) return AZ_PENDING;
    for (uint32_t i = 0; i < existing->count; ++i) {
        const az_cue_entry *p = &existing->entries[i];
        if ((p->classification == AZ_CUE_HOT || p->classification == AZ_CUE_MEMORY) &&
            (!(p->valid & AZ_CUE_START) || !isfinite(p->start_s))) return AZ_STALE;
    }
    int preferred = c->cue_preference == AZ_CUE_MEMORY ? AZ_CUE_MEMORY : AZ_CUE_HOT;
    /* Preferred type first, preserving group/rule/phrase order within each
     * pass. Thus later preferred cues never replace already admitted points. */
    for (int pass = 0; pass < 2; ++pass) {
        int type = pass ? (preferred == AZ_CUE_HOT ? AZ_CUE_MEMORY : AZ_CUE_HOT) : preferred;
        for (int gi = 0; gi < c->group_count; ++gi) {
            const cues_group *g = &c->groups[gi];
            if (g->rule_count < 0 || g->rule_count > CUES_MAX_RULES ||
                g->measures_in < CUES_BARS || g->measures_in > CUES_SECONDS ||
                g->quantization < CUES_INHERIT || g->quantization > CUES_QUANT_BAR ||
                !isfinite(g->min_bpm) || !isfinite(g->max_bpm)) return AZ_INVALID;
            if (!group_matches(c, g, s)) continue;
            for (int ri = 0; ri < g->rule_count; ++ri) {
                const cues_rule *r = &g->rules[ri];
                if (!r->enabled || r->cue_type != type) continue;
                if (r->anchor < CUES_PHRASE || r->anchor > CUES_ABSOLUTE || r->color < 0 || r->color > 8 ||
                    r->pad < 1 || r->pad > 8 || r->unit < CUES_BARS || r->unit > CUES_SECONDS ||
                    r->offset_unit < CUES_BARS || r->offset_unit > CUES_SECONDS ||
                    r->loop_unit < CUES_BARS || r->loop_unit > CUES_SECONDS ||
                    r->phrase_type < 0 || r->phrase_type > 7 || r->phrase_index < 0 || r->phrase_index > 10 ||
                    r->phrase_point < CUES_START || r->phrase_point > CUES_END ||
                    r->loop_mode < CUES_NO_LOOP || r->loop_mode > CUES_LOOP_TO_END ||
                    !isfinite(r->position) || !isfinite(r->offset) || !isfinite(r->loop_length) ||
                    !terminated(r->name, sizeof r->name) || !terminated(r->comment, sizeof r->comment)) return AZ_INVALID;
                if (!room(c, existing, out, type, r->pad)) { ++out->skipped; continue; }
                if (r->anchor == CUES_ABSOLUTE) {
                    if (!make_point(c, g, r, &map, existing, out, 0, 0, 0)) ++out->skipped;
                    continue;
                }
                if (!map.grid_valid || !ready(&s->phrases_info) || !s->phrases || s->phrase_count > 2048) {
                    ++out->skipped;
                    continue;
                }
                unsigned match = 0;
                for (uint32_t pi = 0; pi < s->phrase_count; ++pi) {
                    if (!room(c, existing, out, type, r->pad)) break;
                    const az_phrase *p = &s->phrases[pi];
                    int category = phrase_category(p);
                    if (r->phrase_type && r->phrase_type != category) continue;
                    ++match;
                    if (r->phrase_index && match != (unsigned)r->phrase_index) continue;
                    if (p->start_beat >= p->end_beat || p->end_beat >= s->beat_count ||
                        (pi && p->start_beat < s->phrases[pi - 1].end_beat)) { ++out->skipped; continue; }
                    double start = s->beats[p->start_beat].time_s, end = s->beats[p->end_beat].time_s;
                    if (!make_point(c, g, r, &map, existing, out, category, start, end)) ++out->skipped;
                }
            }
        }
    }
    return AZ_OK;
}
