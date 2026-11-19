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
    static const unsigned char high[] = { 0, 1, 2, 3, 0, 4, 5 };
    static const unsigned char other[] = { 0, 1, 6, 6, 6, 6, 6, 6, 7, 4, 5 };
    if (p->raw_mood == 1) return p->raw_kind < sizeof high ? high[p->raw_kind] : 0;
    if (p->raw_mood == 2 || p->raw_mood == 3)
        return p->raw_kind < sizeof other ? other[p->raw_kind] : 0;
    /* No mood means no trustworthy classification. */
    return 0;
}

static int terminated(const char *s, size_t n) { return memchr(s, 0, n) != NULL; }
static int fold(unsigned char c) { return c < 128 ? tolower(c) : c; }
static int same_text(const char *a, const char *b)
{
    while (*a && *b && fold((unsigned char)*a) == fold((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return !*a && !*b;
}
static int contains_text(const char *haystack, const char *needle)
{
    if (!*needle) return 1;
    for (; *haystack; ++haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n && fold((unsigned char)*h) == fold((unsigned char)*n)) {
            ++h;
            ++n;
        }
        if (!*n) return 1;
    }
    return 0;
}
static int ready(const az_component_info *info) { return info->availability == AZ_AVAIL_READY; }

static int group_artist_matches(const cues_group *g, const az_track_metadata *m)
{
    if (!*g->artist) return 1;
    return (m->valid & AZ_META_ARTIST) && terminated(m->artist, sizeof m->artist) && contains_text(m->artist, g->artist);
}
static int group_key_matches(const cues_group *g, const az_track_metadata *m)
{
    if (!*g->key) return 1;
    return (m->valid & AZ_META_KEY) && terminated(m->key, sizeof m->key) && same_text(m->key, g->key);
}
static int metadata_tempo_valid(const az_track_metadata *m)
{
    return (m->valid & AZ_META_TEMPO) && isfinite(m->original_bpm) && m->original_bpm > 0;
}
static int tempo_in_range(double tempo, double low, double high)
{
    return !(low > 0 && tempo < low) && !(high > 0 && tempo > high);
}
static int group_tempo_matches(const cues_config *c, const cues_group *g, const az_track_metadata *m)
{
    double low = g->min_bpm > 0 ? g->min_bpm : c->min_bpm;
    double high = g->max_bpm > 0 ? g->max_bpm : c->max_bpm;
    if (!(low > 0 || high > 0)) return 1;
    return metadata_tempo_valid(m) && tempo_in_range(m->original_bpm, low, high);
}
static int group_matches(const cues_config *c, const cues_group *g, const az_track_snapshot *s)
{
    const az_track_metadata *m = &s->metadata;
    if (!g->enabled || !terminated(g->artist, sizeof g->artist) || !terminated(g->key, sizeof g->key)) return 0;
    return group_artist_matches(g, m) && group_key_matches(g, m) && group_tempo_matches(c, g, m);
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
static int grid_beat_valid(const az_track_snapshot *s, uint32_t i, uint32_t rate)
{
    int nonnegative = compare_timestamps(s->beats[i].time_s, 0., 1, rate, 1);
    if (nonnegative == AZ_TIME_INVALID || nonnegative == AZ_TIME_BEFORE) return 0;
    if (!i) return 1;
    int order = compare_timestamps(s->beats[i].time_s, s->beats[i - 1].time_s, 1, rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_AFTER;
}
static int grid_valid(const az_track_snapshot *s, uint32_t rate)
{
    if (!ready(&s->grid_info) || !s->beats || s->beat_count < 2 || s->beat_count > 32768) return 0;
    for (uint32_t i = 0; i < s->beat_count; ++i)
        if (!grid_beat_valid(s, i, rate)) return 0;
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
static int beat_time_at(const az_track_snapshot *s, uint32_t index, double fraction, double *time)
{
    if (index >= s->beat_count) return 0;
    if (fraction > 0) {
        if (index + 1 >= s->beat_count) return 0;
        *time = s->beats[index].time_s + fraction * (s->beats[index + 1].time_s - s->beats[index].time_s);
    } else *time = s->beats[index].time_s;
    return isfinite(*time);
}
static int bar_time_at(const clock_map *m, uint32_t index, double fraction, double *time)
{
    const az_track_snapshot *s = m->track;
    if (index >= m->bar_count) return 0;
    uint32_t first = m->bars[index];
    if (!(fraction > 0)) {
        *time = s->beats[first].time_s;
        return 1;
    }
    if (index + 1 >= m->bar_count) return 0;
    uint32_t next = m->bars[index + 1];
    *time = s->beats[first].time_s + fraction * (s->beats[next].time_s - s->beats[first].time_s);
    return isfinite(*time);
}
static int time_at(const clock_map *m, int unit, double coordinate, double *time)
{
    if (!isfinite(coordinate)) return 0;
    if (unit == CUES_SECONDS) {
        *time = coordinate;
        return isfinite(*time);
    }
    if (!m->grid_valid || coordinate < 0) return 0;
    double whole = floor(coordinate), fraction = coordinate - whole;
    if (whole > m->track->beat_count) return 0;
    uint32_t index = (uint32_t)whole;
    if (unit == CUES_BEATS) return beat_time_at(m->track, index, fraction, time);
    if (unit != CUES_BARS) return 0;
    return bar_time_at(m, index, fraction, time);
}
static uint32_t coordinate_beat(const clock_map *m, int unit, uint32_t index)
{
    return unit == CUES_BARS ? m->bars[index] : index;
}
static uint32_t coordinate_count(const clock_map *m, int unit)
{
    return unit == CUES_BARS ? m->bar_count : m->track->beat_count;
}
/* AZ_OK identifies an exact coordinate. AZ_NOT_FOUND returns the insertion
 * index for interpolation; invalid clock comparisons leave output unchanged. */
static int coordinate_search(const clock_map *m, int unit, double time, uint32_t *lower)
{
    uint32_t low = 0, high = coordinate_count(m, unit);
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint32_t index = coordinate_beat(m, unit, middle);
        int order = compare_timestamps(time, m->track->beats[index].time_s, 1, m->rate, 1);
        if (order == AZ_TIME_INVALID) return AZ_INVALID;
        if (order == AZ_TIME_EQUAL) {
            *lower = middle;
            return AZ_OK;
        }
        if (order == AZ_TIME_BEFORE) high = middle;
        else low = middle + 1;
    }
    *lower = low;
    return AZ_NOT_FOUND;
}
static int coordinate_between(const clock_map *m, int unit, double time, uint32_t low, double *coordinate)
{
    if (!low || low == coordinate_count(m, unit)) return 0;
    uint32_t previous = coordinate_beat(m, unit, low - 1), next = coordinate_beat(m, unit, low);
    const az_track_snapshot *s = m->track;
    *coordinate = low - 1 + (time - s->beats[previous].time_s) / (s->beats[next].time_s - s->beats[previous].time_s);
    return isfinite(*coordinate);
}
static int coordinate_at(const clock_map *m, int unit, double time, double *coordinate)
{
    if (!isfinite(time)) return 0;
    if (unit == CUES_SECONDS) {
        *coordinate = time;
        return 1;
    }
    if (!m->grid_valid || (unit != CUES_BEATS && unit != CUES_BARS)) return 0;
    uint32_t low;
    int rc = coordinate_search(m, unit, time, &low);
    if (rc == AZ_INVALID) return 0;
    if (rc == AZ_OK) {
        *coordinate = low;
        return 1;
    }
    return coordinate_between(m, unit, time, low, coordinate);
}
static int shift(const clock_map *m, double base, double amount, int unit, double *out)
{
    double coordinate;
    return coordinate_at(m, unit, base, &coordinate) && time_at(m, unit, coordinate + amount, out);
}
static int nearest_boundary(const clock_map *m, double input, double lo, double hi, double *out)
{
    /* Nearest in source time; an exact tie goes to the earlier boundary. */
    int near = compare_timestamps(input - lo, hi - input, 1, m->rate, 1);
    if (near == AZ_TIME_INVALID) return 0;
    *out = near == AZ_TIME_AFTER ? hi : lo;
    return 1;
}
static int quantize(const clock_map *m, int mode, double input, double *out)
{
    if (mode == CUES_QUANT_OFF) {
        *out = input;
        return isfinite(input);
    }
    if (!m->grid_valid || (mode != CUES_QUANT_BEAT && mode != CUES_QUANT_BAR)) return 0;
    int unit = mode == CUES_QUANT_BAR ? CUES_BARS : CUES_BEATS;
    double coordinate, lo, hi;
    if (!coordinate_at(m, unit, input, &coordinate) || !time_at(m, unit, floor(coordinate), &lo)) return 0;
    if (!time_at(m, unit, ceil(coordinate), &hi)) return 0;
    return nearest_boundary(m, input, lo, hi, out);
}

/* Copy complete UTF-8 codepoints only; configuration strings were validated. */
static size_t utf8_sequence_size(unsigned char c)
{
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 0;
}
static size_t utf8_width(const char *s)
{
    unsigned char c = (unsigned char)*s;
    size_t n = utf8_sequence_size(c);
    if (!n) return 0;
    for (size_t i = 1; i < n; ++i)
        if (((unsigned char)s[i] & 0xc0) != 0x80) return 0;
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
        if (!strncmp(pattern, "{phrase}", 8)) {
            append(out, size, phrase);
            pattern += 8;
        } else if (!strncmp(pattern, "{length}", 8)) {
            append(out, size, length);
            pattern += 8;
        } else {
            size_t n = utf8_width(pattern);
            if (!n) break;
            char one[5] = { 0 };
            memcpy(one, pattern, n);
            if (strlen(out) + n >= size) break;
            append(out, size, one);
            pattern += n;
        }
    }
}
static void label_length(char *length, size_t capacity, const cues_point *point,
                         const cues_rule *r, const cues_group *g, const clock_map *m,
                         double phrase_start, double phrase_end)
{
    double start = r->anchor == CUES_PHRASE ? phrase_start : point->start_s;
    double end = r->anchor == CUES_PHRASE ? phrase_end : point->end_s;
    if (r->anchor == CUES_PHRASE || point->type == AZ_CUE_LOOP) {
        double a, b;
        if (coordinate_at(m, g->measures_in, start, &a) && coordinate_at(m, g->measures_in, end, &b))
            snprintf(length, capacity, "%.6g", b - a);
        else snprintf(length, capacity, "?");
    }
}
static void label(cues_point *point, const cues_rule *r, const cues_group *g,
                  const clock_map *m, int category, double phrase_start, double phrase_end)
{
    char name[128], comment[128], length[32] = "0";
    label_length(length, sizeof length, point, r, g, m, phrase_start, phrase_end);
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
static int hot_slot_taken(int classification, unsigned slot, unsigned occupied_class, unsigned occupied_slot)
{
    return classification == AZ_CUE_HOT && occupied_class == AZ_CUE_HOT && occupied_slot == slot;
}
static int slot_allowed(const cues_config *c, int classification, int slot)
{
    return classification != AZ_CUE_HOT || (slot >= 1 && slot <= c->hot_pads);
}
static int room(const cues_config *c, const az_native_cue_snapshot *existing,
                const cues_plan *out, int classification, int slot)
{
    int memory = 0;
    if (out->count >= CUES_MAX_PLAN) return 0;
    if (!slot_allowed(c, classification, slot)) return 0;
    for (uint32_t i = 0; i < existing->count; ++i) {
        const az_cue_entry *p = &existing->entries[i];
        memory += p->classification == AZ_CUE_MEMORY;
        if (hot_slot_taken(classification, (unsigned)slot, p->classification, p->slot)) return 0;
    }
    for (int i = 0; i < out->count; ++i) {
        const cues_point *p = &out->points[i];
        memory += p->classification == AZ_CUE_MEMORY;
        if (hot_slot_taken(classification, (unsigned)slot, p->classification, p->slot)) return 0;
    }
    return classification == AZ_CUE_MEMORY ? memory < c->max_memory && memory < 10 : 1;
}
static int rule_slot(const cues_config *c, const az_native_cue_snapshot *existing,
                     const cues_plan *out, const cues_rule *r)
{
    if (!cues_rule_auto_pad(r)) return r->pad;
    for (int slot = 1; slot <= c->hot_pads; ++slot)
        if (room(c, existing, out, AZ_CUE_HOT, slot)) return slot;
    return 0;
}
static int existing_point_near(const az_cue_entry *p, const cues_point *candidate, double distance, uint32_t rate)
{
    return (p->classification == AZ_CUE_HOT || p->classification == AZ_CUE_MEMORY) &&
           (p->valid & AZ_CUE_START) && near_point(p->start_s, candidate->start_s, distance, rate);
}
static int candidate_capacity(const cues_config *c, const cues_point *candidate, int memory, int hot)
{
    if (candidate->classification == AZ_CUE_MEMORY) return memory < c->max_memory && memory < 10;
    return hot < 8 && candidate->slot <= (unsigned)c->hot_pads;
}
static int insert(const cues_config *c, const az_native_cue_snapshot *existing,
                  uint32_t rate, cues_plan *out, const cues_point *candidate)
{
    int memory = 0, hot = 0;
    for (uint32_t i = 0; i < existing->count; ++i) {
        const az_cue_entry *p = &existing->entries[i];
        memory += p->classification == AZ_CUE_MEMORY;
        hot += p->classification == AZ_CUE_HOT;
        if (hot_slot_taken(candidate->classification, candidate->slot, p->classification, p->slot)) return 0;
        if (existing_point_near(p, candidate, c->min_distance, rate)) return 0;
    }
    for (int i = 0; i < out->count; ++i) {
        const cues_point *p = &out->points[i];
        memory += p->classification == AZ_CUE_MEMORY;
        hot += p->classification == AZ_CUE_HOT;
        if (hot_slot_taken(candidate->classification, candidate->slot, p->classification, p->slot)) return 0;
        if (near_point(p->start_s, candidate->start_s, c->min_distance, rate)) return 0;
    }
    if (!candidate_capacity(c, candidate, memory, hot)) return 0;
    if (out->count >= CUES_MAX_PLAN) return 0;
    out->points[out->count++] = *candidate;
    return 1;
}
static int point_anchor(const cues_rule *r, const clock_map *m, double start, double end, double *anchor)
{
    if (r->anchor == CUES_ABSOLUTE) {
        if (!time_at(m, r->unit, r->position, anchor)) return 0;
    } else if (r->phrase_point == CUES_START) *anchor = start;
    else if (r->phrase_point == CUES_MIDDLE) *anchor = (start + end) / 2;
    else *anchor = end;
    return 1;
}
static int point_loop_bounds(const cues_rule *r, const clock_map *m, int quant, double start, double end, cues_point *p)
{
    if (r->loop_mode == CUES_LOOP_FIXED)
        return r->loop_length > 0 && shift(m, p->start_s, r->loop_length, r->loop_unit, &p->end_s);
    if (r->anchor != CUES_PHRASE) return 0;
    if (r->loop_mode == CUES_LOOP_FROM_START) {
        p->end_s = p->start_s;
        return quantize(m, quant, start, &p->start_s);
    }
    if (r->loop_mode == CUES_LOOP_TO_END) return quantize(m, quant, end, &p->end_s);
    return 0;
}
static int point_loop(const cues_rule *r, const clock_map *m, int quant, double start, double end, cues_point *p)
{
    p->type = AZ_CUE_POINT;
    if (r->loop_mode == CUES_NO_LOOP) return 1;
    p->type = AZ_CUE_LOOP;
    if (!point_loop_bounds(r, m, quant, start, end, p)) return 0;
    int order = compare_timestamps(p->end_s, p->start_s, 1, m->rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_AFTER && time_range(m, p->end_s);
}
static int point_offset(const cues_rule *r, const clock_map *m, double *anchor)
{
    /* Zero is the identity operation even without a provider bar model. */
    if (r->anchor != CUES_PHRASE || !r->offset) return 1;
    return shift(m, *anchor, r->offset, r->offset_unit, anchor);
}
static int make_point(const cues_config *c, const cues_group *g, const cues_rule *r,
                      const clock_map *m, const az_native_cue_snapshot *existing,
                      cues_plan *out, int slot, int category, double start, double end)
{
    cues_point p = { 0 };
    double anchor = 0;
    if (!point_anchor(r, m, start, end, &anchor)) return 0;
    if (!point_offset(r, m, &anchor)) return 0;
    int quant = g->quantization == CUES_INHERIT ? c->quantization : g->quantization;
    if (!quantize(m, quant, anchor, &p.start_s)) return 0;
    if (!point_loop(r, m, quant, start, end, &p)) return 0;
    if (!time_range(m, p.start_s)) return 0;
    p.classification = (uint32_t)r->cue_type;
    p.slot = r->cue_type == AZ_CUE_HOT ? (uint32_t)slot : 0;
    p.rgb = cues_colors[r->color];
    label(&p, r, g, m, category, start, end);
    return insert(c, existing, m->rate, out, &p);
}

/* Validate each contract before constructing points. Invalid input keeps the
 * same public status; pending phrase data clears any partial absolute plan. */
static int plan_config_limits_valid(const cues_config *c)
{
    return c->group_count >= 0 && c->group_count <= CUES_MAX_GROUPS &&
           c->max_memory >= 0 && c->max_memory <= 10 && c->hot_pads >= 1 && c->hot_pads <= 8;
}
static int plan_config_measures_valid(const cues_config *c)
{
    return c->quantization >= CUES_QUANT_OFF && c->quantization <= CUES_QUANT_BAR &&
           c->measures_in >= CUES_BARS && c->measures_in <= CUES_SECONDS &&
           isfinite(c->min_distance) && c->min_distance >= 0 && isfinite(c->min_bpm) && isfinite(c->max_bpm);
}
static int plan_snapshot_valid(const az_track_snapshot *s, const az_native_cue_snapshot *existing)
{
    return s->size >= sizeof *s && s->version == AZ_DATA_VERSION &&
           existing->size >= sizeof *existing && existing->version == AZ_DATA_VERSION &&
           existing->revision && existing->native_load_generation && existing->count <= AZ_NATIVE_CUE_LIMIT;
}
static int plan_inputs_valid(const cues_config *c, const az_track_snapshot *s,
                             const az_native_cue_snapshot *existing, uint32_t sample_rate)
{
    return c && s && existing && sample_rate && sample_rate < 0x7fffffffu &&
           plan_snapshot_valid(s, existing) && plan_config_limits_valid(c) && plan_config_measures_valid(c);
}
static void plan_clock(clock_map *map)
{
    const az_track_snapshot *s = map->track;
    map->grid_valid = grid_valid(s, map->rate);
    if (map->grid_valid)
        for (uint32_t i = 0; i < s->beat_count; ++i)
            if (s->beats[i].beat_in_bar == 1) map->bars[map->bar_count++] = (uint16_t)i;
    int order = compare_timestamps(s->metadata.duration_s, 0., 1, map->rate, 1);
    map->duration_valid = (s->metadata.valid & AZ_META_DURATION) &&
                          order != AZ_TIME_INVALID && order == AZ_TIME_AFTER;
}
static int plan_existing_valid(const az_native_cue_snapshot *existing)
{
    for (uint32_t i = 0; i < existing->count; ++i) {
        const az_cue_entry *p = &existing->entries[i];
        if ((p->classification == AZ_CUE_HOT || p->classification == AZ_CUE_MEMORY) &&
            (!(p->valid & AZ_CUE_START) || !isfinite(p->start_s))) return 0;
    }
    return 1;
}
static int plan_group_valid(const cues_group *g)
{
    return g->rule_count >= 0 && g->rule_count <= CUES_MAX_RULES &&
           g->measures_in >= CUES_BARS && g->measures_in <= CUES_SECONDS &&
           g->quantization >= CUES_INHERIT && g->quantization <= CUES_QUANT_BAR &&
           isfinite(g->min_bpm) && isfinite(g->max_bpm);
}
static int plan_rule_placement_valid(const cues_rule *r)
{
    return r->anchor >= CUES_PHRASE && r->anchor <= CUES_ABSOLUTE &&
           r->color >= 0 && r->color <= 8 && r->pad >= 1 && r->pad <= 8;
}
static int plan_rule_units_valid(const cues_rule *r)
{
    return r->unit >= CUES_BARS && r->unit <= CUES_SECONDS &&
           r->offset_unit >= CUES_BARS && r->offset_unit <= CUES_SECONDS &&
           r->loop_unit >= CUES_BARS && r->loop_unit <= CUES_SECONDS;
}
static int plan_rule_phrase_valid(const cues_rule *r)
{
    return r->phrase_type >= 0 && r->phrase_type <= 7 && r->phrase_index >= 0 && r->phrase_index <= 10 &&
           r->phrase_point >= CUES_START && r->phrase_point <= CUES_END &&
           r->loop_mode >= CUES_NO_LOOP && r->loop_mode <= CUES_LOOP_TO_END;
}
static int plan_rule_valid(const cues_rule *r)
{
    return plan_rule_placement_valid(r) && plan_rule_units_valid(r) && plan_rule_phrase_valid(r) &&
           isfinite(r->position) && isfinite(r->offset) && isfinite(r->loop_length) &&
           terminated(r->name, sizeof r->name) && terminated(r->comment, sizeof r->comment);
}
struct plan_builder {
    const cues_config *config;
    const az_track_snapshot *track;
    const az_native_cue_snapshot *existing;
    const clock_map *map;
    cues_plan *out;
};
static int plan_phrase_selected(const cues_rule *r, int category, unsigned *match)
{
    if (r->phrase_type && r->phrase_type != category) return 0;
    ++*match;
    return !r->phrase_index || *match == (unsigned)r->phrase_index;
}
static int plan_phrase_range_valid(const az_track_snapshot *s, uint32_t pi)
{
    const az_phrase *p = &s->phrases[pi];
    return p->start_beat < p->end_beat && p->end_beat < s->beat_count &&
           (!pi || p->start_beat >= s->phrases[pi - 1].end_beat);
}
static void plan_phrase_points(const struct plan_builder *b, const cues_group *g, const cues_rule *r)
{
    const az_track_snapshot *s = b->track;
    unsigned match = 0;
    for (uint32_t pi = 0; pi < s->phrase_count; ++pi) {
        int slot = rule_slot(b->config, b->existing, b->out, r);
        if (!room(b->config, b->existing, b->out, r->cue_type, slot)) break;
        const az_phrase *p = &s->phrases[pi];
        int category = phrase_category(p);
        if (!plan_phrase_selected(r, category, &match)) continue;
        if (!plan_phrase_range_valid(s, pi)) {
            ++b->out->skipped;
            continue;
        }
        double start = s->beats[p->start_beat].time_s, end = s->beats[p->end_beat].time_s;
        if (!make_point(b->config, g, r, b->map, b->existing, b->out, slot, category, start, end)) ++b->out->skipped;
    }
}
static int plan_phrases_ready(const struct plan_builder *b)
{
    return b->map->grid_valid && ready(&b->track->phrases_info) && b->track->phrases && b->track->phrase_count;
}
static int plan_rule(const struct plan_builder *b, const cues_group *g, const cues_rule *r)
{
    if (!plan_rule_valid(r)) return AZ_INVALID;
    /* A newly loaded empty phrase bank must not consume automatic placement
     * with an absolute subset before Track Analysis publishes its result. */
    if (r->anchor == CUES_PHRASE && !plan_phrases_ready(b)) {
        memset(b->out, 0, sizeof *b->out);
        return AZ_PENDING;
    }
    int slot = rule_slot(b->config, b->existing, b->out, r);
    if (!room(b->config, b->existing, b->out, r->cue_type, slot)) {
        ++b->out->skipped;
        return AZ_OK;
    }
    if (r->anchor == CUES_ABSOLUTE) {
        if (!make_point(b->config, g, r, b->map, b->existing, b->out, slot, 0, 0, 0)) ++b->out->skipped;
        return AZ_OK;
    }
    if (!plan_phrases_ready(b) || b->track->phrase_count > 2048) {
        ++b->out->skipped;
        return AZ_OK;
    }
    plan_phrase_points(b, g, r);
    return AZ_OK;
}
static int plan_group(const struct plan_builder *b, const cues_group *g, int type)
{
    if (!plan_group_valid(g)) return AZ_INVALID;
    if (!group_matches(b->config, g, b->track)) return AZ_OK;
    for (int ri = 0; ri < g->rule_count; ++ri) {
        const cues_rule *r = &g->rules[ri];
        if (!r->enabled || r->cue_type != type) continue;
        int rc = plan_rule(b, g, r);
        if (rc != AZ_OK) return rc;
    }
    return AZ_OK;
}
static int plan_type(const struct plan_builder *b, int type)
{
    for (int gi = 0; gi < b->config->group_count; ++gi) {
        int rc = plan_group(b, &b->config->groups[gi], type);
        if (rc != AZ_OK) return rc;
    }
    return AZ_OK;
}
static int plan_preferred_first(const struct plan_builder *b)
{
    int preferred = b->config->cue_preference == AZ_CUE_MEMORY ? AZ_CUE_MEMORY : AZ_CUE_HOT;
    int rc = plan_type(b, preferred);
    if (rc != AZ_OK) return rc;
    return plan_type(b, preferred == AZ_CUE_HOT ? AZ_CUE_MEMORY : AZ_CUE_HOT);
}
int cues_build_plan(const cues_config *c, const az_track_snapshot *s,
                    const az_native_cue_snapshot *existing, uint32_t sample_rate, cues_plan *out)
{
    if (!out) return AZ_INVALID;
    memset(out, 0, sizeof *out);
    if (!plan_inputs_valid(c, s, existing, sample_rate)) return AZ_INVALID;
    if (!c->enabled) return AZ_OK;
    if (!ready(&s->metadata_info)) return AZ_PENDING;
    clock_map map = { .track = s, .rate = sample_rate };
    plan_clock(&map);
    if (!map.duration_valid) return AZ_PENDING;
    if (!plan_existing_valid(existing)) return AZ_STALE;
    const struct plan_builder builder = { c, s, existing, &map, out };
    return plan_preferred_first(&builder);
}
