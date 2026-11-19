/* SPDX-License-Identifier: GPL-3.0-only
 *
 * guide.c: target selection and the transition plan.
 */
#include "guide.h"
#include "azmod-timestamps.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t n, const char *s)
{
    if (!n) return;
    snprintf(dst, n, "%s", s ? s : "");
}

static void fmt_bars(char *buf, size_t n, double ms, uint32_t bar_ms)
{
    if (ms <= -200) {
        snprintf(buf, n, "LATE");
        return;
    }
    if (ms < 0) ms = 0;
    if (bar_ms < 200) {
        snprintf(buf, n, "--");
        return;
    }
    double bar = bar_ms;
    if (ms < 1800)
        snprintf(buf, n, "%.0f BEATS", ms / (bar / 4.0));
    else if (ms < bar * 9.95)
        snprintf(buf, n, "%.1f BARS", ms / bar);
    else
        snprintf(buf, n, "%.0f BARS", ms / bar);
}

static int same_target(uint32_t a, uint32_t b, uint32_t sample_rate)
{
    int order = compare_timestamps(a, b, 1000, sample_rate, 1);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL;
}

/* Add a candidate, keeping the array sorted and dropping duplicates from another
 * source (a phrase end and a cue on the same beat, say). */
static int candidate_lower_bound(const an_cand_t *out, int n, uint32_t ms)
{
    int at = 0, hi = n;
    while (at < hi) {
        int mid = at + (hi - at) / 2;
        if (out[mid].ms < ms) at = mid + 1;
        else hi = mid;
    }
    return at;
}

static void add_cand(an_cand_t *out, int *n, int max, uint32_t ms, uint32_t sample_rate,
                     const char *label)
{
    if (*n >= max) return;
    int at = candidate_lower_bound(out, *n, ms);
    if ((at < *n && same_target(out[at].ms, ms, sample_rate)) ||
        (at > 0 && same_target(out[at - 1].ms, ms, sample_rate))) return;
    memmove(&out[at + 1], &out[at], (size_t)(*n - at) * sizeof *out);
    out[at].ms = ms;
    copy_str(out[at].label, sizeof out[at].label, label);
    (*n)++;
}

static int grid_next(const an_analysis_t *a, uint32_t pos_ms, int bars, uint32_t *when, int maxn)
{
    if (!a || !a->nbeats || bars <= 0) return 0;
    int stride = bars * 4;
    int found = 0;
    for (int i = 0; i < a->nbeats && found < maxn; i++) {
        if (i % stride != 0) continue;
        if (a->beats[i].beat_in_bar != 1) continue;
        if (a->beats[i].time_ms <= pos_ms + 250) continue;
        when[found++] = a->beats[i].time_ms;
    }
    return found;
}

int an_phrase_category(uint32_t mood, uint32_t kind)
{
    static const int high[] = { AN_CAT_OTHER, AN_CAT_INTRO, AN_CAT_BUILD, AN_CAT_BREAK,
                                AN_CAT_OTHER, AN_CAT_CHORUS, AN_CAT_OUTRO, AN_CAT_OTHER, AN_CAT_OTHER, AN_CAT_OTHER, AN_CAT_OTHER };
    static const int other[] = { AN_CAT_OTHER, AN_CAT_INTRO, AN_CAT_VERSE, AN_CAT_VERSE,
                                 AN_CAT_VERSE, AN_CAT_VERSE, AN_CAT_VERSE, AN_CAT_VERSE, AN_CAT_BRIDGE, AN_CAT_CHORUS, AN_CAT_OUTRO };
    static const int unknown[] = { AN_CAT_OTHER, AN_CAT_INTRO, AN_CAT_OTHER, AN_CAT_OTHER,
                                   AN_CAT_OTHER, AN_CAT_CHORUS, AN_CAT_OUTRO, AN_CAT_OTHER, AN_CAT_BRIDGE, AN_CAT_CHORUS, AN_CAT_OUTRO };
    if (kind >= sizeof high / sizeof high[0]) return AN_CAT_OTHER;
    if (mood == 1) return high[kind];
    return mood == 2 || mood == 3 ? other[kind] : unknown[kind];
}

const char *an_category_name(int category)
{
    switch (category) {
    case AN_CAT_INTRO: return "INTRO";
    case AN_CAT_BUILD: return "BUILD";
    case AN_CAT_BREAK: return "BREAK";
    case AN_CAT_CHORUS: return "CHORUS";
    case AN_CAT_OUTRO: return "OUTRO";
    case AN_CAT_VERSE: return "VERSE";
    case AN_CAT_BRIDGE: return "BRIDGE";
    default: return "PHRASE";
    }
}

static int joins_group(const an_group_t *groups, int n, const an_phrase_t *phrase)
{
    return n > 0 && groups[n - 1].category == phrase->category && phrase->joins_previous;
}

int an_phrase_groups(const an_analysis_t *a, int consolidate, an_group_t *out, int max)
{
    int n = 0;
    if (!a || !a->nphrases || !out || max <= 0) return 0;
    for (int i = 0; i < a->nphrases; i++) {
        const an_phrase_t *phrase = &a->phrases[i];
        if (consolidate && joins_group(out, n, phrase)) {
            out[n - 1].end_ms = phrase->end_ms;
            continue;
        }
        if (n >= max) break;
        out[n].category = phrase->category;
        out[n].start_ms = phrase->start_ms;
        out[n].end_ms = phrase->end_ms;
        n++;
    }
    return n;
}

static void label_group(char *dst, size_t n, const an_group_t *g, int start_point)
{
    snprintf(dst, n, "%s %s", an_category_name(g->category), start_point ? "START" : "END");
}

/* The phrase group the playhead is in, or the last one before it. */
static int group_at(const an_group_t *g, int n, uint32_t pos_ms)
{
    int at = -1;
    for (int i = 0; i < n; i++) {
        if (g[i].start_ms <= pos_ms) at = i;
        else break;
    }
    return at;
}

static int target_group(const an_group_t *g, int i, int start_point, an_cand_t *out)
{
    memset(out, 0, sizeof *out);
    out->ms = start_point ? g[i].start_ms : g[i].end_ms;
    label_group(out->label, sizeof out->label, &g[i], start_point);
    return 0;
}

/* Grid boundary fallback, and the grid rules. */
static int target_grid(const an_analysis_t *a, uint32_t pos_ms, int bars, an_cand_t *out)
{
    uint32_t when[1];
    if (!a || grid_next(a, pos_ms, bars, when, 1) <= 0) return -1;
    memset(out, 0, sizeof *out);
    out->ms = when[0];
    snprintf(out->label, sizeof out->label, "%d-BAR", bars);
    return 0;
}

static void hotcue_label(const an_cue_t *c, char *label, size_t size)
{
    char slot[8];
    snprintf(slot, sizeof slot, "HC%u", c->slot);
    if (c->label[0] && strncmp(c->label, slot, strlen(slot)) != 0)
        snprintf(label, size, "%s %.24s", slot, c->label);
    else
        snprintf(label, size, "%s", c->label[0] ? c->label : slot);
}

static int auto_hotcue(const an_analysis_t *a, uint32_t pos_ms, const an_target_opts_t *opts, an_cand_t *out)
{
    for (int i = 0; i < a->ncues; i++) {
        const an_cue_t *c = &a->cues[i];
        if (!c->slot) continue;
        if ((int64_t)c->time_ms <= (int64_t)pos_ms + 150) continue;
        if (!opts->hotcue_any && c->slot != (uint16_t)opts->hotcue_slot) continue;
        out->ms = c->time_ms;
        hotcue_label(c, out->label, sizeof out->label);
        return 0;
    }
    return -1;
}

static int target_category(const an_group_t *g, int n, int at, int cat, int start_point, uint32_t pos_ms, an_cand_t *out)
{
    for (int i = (at > 0 ? at : 0); i < n; i++) {
        if (g[i].category != cat) continue;
        uint32_t when = start_point ? g[i].start_ms : g[i].end_ms;
        if ((int64_t)when <= (int64_t)pos_ms + 150) continue;
        return target_group(g, i, start_point, out);
    }
        /* no phrase of the chosen kind ahead: fall through to the current group so the
         * guide still has a target, rather than going blank on an unusual track */
    return -1;
}

static int target_from_group(const an_group_t *g, int n, int from, int start_point, uint32_t pos_ms, an_cand_t *out)
{
    for (int i = from; i < n; i++) {
        uint32_t when = start_point ? g[i].start_ms : g[i].end_ms;
        if ((int64_t)when > (int64_t)pos_ms + 150) return target_group(g, i, start_point, out);
    }
    return -1;
}

static int auto_phrase(const an_analysis_t *a, uint32_t pos_ms, const an_target_opts_t *opts, an_cand_t *out)
{
    an_group_t g[AN_MAX_PHRASES];
    int n = an_phrase_groups(a, opts->consolidate, g, AN_MAX_PHRASES);
    int start_point = opts->phrase_point == AN_POINT_START;
    if (n <= 0) return target_grid(a, pos_ms, 16, out);

    int at = group_at(g, n, pos_ms);
    int cat = opts->phrase_cat;

    if (cat >= 0 && target_category(g, n, at, cat, start_point, pos_ms, out) == 0) return 0;

    int from = start_point ? at + 1 : at;
    if (from < 0) from = 0;
    if (target_from_group(g, n, from, start_point, pos_ms, out) == 0) return 0;
    return target_from_group(g, n, 0, start_point, pos_ms, out);
}

int an_auto_target(const an_analysis_t *a, uint32_t pos_ms, const an_target_opts_t *opts,
                   an_cand_t *out)
{
    memset(out, 0, sizeof *out);
    if (!a || !opts) return -1;

    if (opts->rule == AN_RULE_HOTCUE) return auto_hotcue(a, pos_ms, opts, out);
    if (opts->rule == AN_RULE_GRID16 || opts->rule == AN_RULE_GRID32)
        return target_grid(a, pos_ms, opts->rule == AN_RULE_GRID32 ? 32 : 16, out);

    return auto_phrase(a, pos_ms, opts, out);
}

static int phrase_candidates(const an_analysis_t *a, uint32_t pos_ms, uint32_t sample_rate, const an_target_opts_t *opts, an_cand_t *out, int max, int *n)
{
    int start_point = opts->phrase_point == AN_POINT_START;

    an_group_t g[AN_MAX_PHRASES];
    int ng = an_phrase_groups(a, opts->consolidate, g, AN_MAX_PHRASES);
    for (int i = 0; i < ng && *n < max; i++) {
        uint32_t when = start_point ? g[i].start_ms : g[i].end_ms;
        if ((int64_t)when <= (int64_t)pos_ms + 250) continue;
        char label[40];
        label_group(label, sizeof label, &g[i], start_point);
        add_cand(out, n, max, when, sample_rate, label);
    }
    return ng;
}

static void cue_candidates(const an_analysis_t *a, uint32_t pos_ms, uint32_t sample_rate, an_cand_t *out, int max, int *n)
{
    for (int i = 0; i < a->ncues && *n < max; i++) {
        if ((int64_t)a->cues[i].time_ms <= (int64_t)pos_ms + 250) continue;
        char label[40];
        const an_cue_t *c = &a->cues[i];
        if (c->slot) {
            hotcue_label(c, label, sizeof label);
        } else {
            snprintf(label, sizeof label, "%s", c->label[0] ? c->label : "CUE");
        }
        add_cand(out, n, max, c->time_ms, sample_rate, label);
    }
}

static void grid_candidates(const an_analysis_t *a, uint32_t pos_ms, uint32_t sample_rate, int rule, an_cand_t *out, int max, int *n)
{
    int bars = rule == AN_RULE_GRID32 ? 32 : 16;
    uint32_t when[4];
    int got = grid_next(a, pos_ms, bars, when, 3);
    for (int i = 0; i < got && *n < max; i++) {
        char label[24];
        snprintf(label, sizeof label, "%d-BAR", bars);
        add_cand(out, n, max, when[i], sample_rate, label);
    }
}

int an_candidates(const an_analysis_t *a, uint32_t pos_ms, uint32_t sample_rate,
                  const an_target_opts_t *opts, an_cand_t *out, int max)
{
    int n = 0;
    if (!a || !out || max <= 0 || !opts) return 0;
    int ng = phrase_candidates(a, pos_ms, sample_rate, opts, out, max, &n);
    cue_candidates(a, pos_ms, sample_rate, out, max, &n);
    if (n < max && (ng == 0 || opts->rule == AN_RULE_GRID16 || opts->rule == AN_RULE_GRID32)) {
        grid_candidates(a, pos_ms, sample_rate, opts->rule, out, max, &n);
    }
    return n;
}

static int cue_drop(const an_analysis_t *b, uint32_t pos_ms, an_cand_t *out)
{
    for (int i = 0; i < b->ncues; i++) {
        if (b->cues[i].time_ms + 200 < pos_ms) continue;
        out->ms = b->cues[i].time_ms;
        if (b->cues[i].slot)
            snprintf(out->label, sizeof out->label, "HC%u %.24s", b->cues[i].slot, b->cues[i].label);
        else
            snprintf(out->label, sizeof out->label, "CUE %.32s", b->cues[i].label);
        return 0;
    }
    return -1;
}

int an_find_drop(const an_analysis_t *b, uint32_t pos_ms, an_cand_t *out)
{
    memset(out, 0, sizeof *out);
    if (!b) return -1;
    const an_phrase_t *first = NULL;
    for (int i = 0; i < b->nphrases; i++) {
        const an_phrase_t *phrase = &b->phrases[i];
        if ((int64_t)phrase->start_ms + 250 < (int64_t)pos_ms) continue;
        if (!first) first = phrase;
        if (phrase->category != AN_CAT_CHORUS) continue;
        out->ms = phrase->start_ms;
        copy_str(out->label, sizeof out->label, an_category_name(phrase->category));
        return 0;
    }
    if (first) {
        out->ms = first->start_ms;
        copy_str(out->label, sizeof out->label, "PHRASE");
        return 0;
    }
    return cue_drop(b, pos_ms, out);
}

double an_grid_bpm_at(const an_analysis_t *a, uint32_t pos_ms)
{
    if (!a->nbeats) return 0;
    int lo = 0, hi = a->nbeats - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (a->beats[mid].time_ms <= pos_ms) lo = mid;
        else hi = mid - 1;
    }
    return a->beats[lo].tempo / 100.0;
}

static void set_action(an_plan_t *p, int state, int action, int accent, const char *line2,
                       double action_ms, uint32_t bar_ms, uint64_t wall)
{
    p->state = state;
    p->action = action;
    p->accent = accent;
    p->valid = 1;
    copy_str(p->line2, sizeof p->line2, line2);
    p->bars_action = action_ms / (double)bar_ms;
    p->action_wall_ms = wall;
    if (action == 1 && action_ms <= 0 && action_ms >= -AN_PLAY_GRACE_MS) {
        p->play_now = 1;
        p->accent = 4;
    }
}

typedef struct {
    double rate, target_wall, exit_wall, tolerance;
    uint32_t bar_ms;
    uint64_t now_ms;
} plan_timing_t;

static void initialize_plan(const an_deck_t *b, const an_loop_t *loop, const an_cand_t *b_target, an_plan_t *out)
{
    memset(out, 0, sizeof *out);
    out->looping = b && b->playing && loop && loop->valid;
    out->loop_prepared = b && !b->playing && loop && loop->valid;
    out->state = AN_ST_NONE;
    if (b_target) {
        out->b_target_valid = 1;
        out->b_target_ms = b_target->ms;
        copy_str(out->b_target, sizeof out->b_target, b_target->label);
    }
}

static int loaded_plan(const an_deck_t *a, const an_deck_t *b, an_plan_t *out)
{
    if (!a || !b || !a->loaded || !b->loaded) {
        out->state = AN_ST_NEED_TRACKS;
        copy_str(out->line1, sizeof out->line1, "WAITING FOR TWO DECKS");
        copy_str(out->line2, sizeof out->line2, "LOAD A AND B IN THE 2-DECK VIEW");
        out->accent = 0;
        return 0;
    }
    return 1;
}

static int outgoing_target_ready(const an_deck_t *a, uint32_t target_ms, an_plan_t *out)
{
    if (!target_ms || (int64_t)target_ms <= (int64_t)a->pos_ms + 150) {
        out->state = AN_ST_NONE;
        copy_str(out->line1, sizeof out->line1, "NO TARGET AHEAD");
        copy_str(out->line2, sizeof out->line2, "TAP TO PICK ANOTHER TARGET");
        out->accent = 0;
        return 0;
    }

    return 1;
}

static int incoming_target_ready(const an_deck_t *b, const an_loop_t *loop, const an_cand_t *b_target, an_plan_t *out)
{
    an_cand_t drop;
    if (!b_target) {
        out->notice = AN_NOTICE_NO_DROP;
        out->state = AN_ST_NO_ANALYSIS;
        copy_str(out->line1, sizeof out->line1, "B HAS NO DROP AHEAD");
        copy_str(out->line2, sizeof out->line2, "CUE B BEFORE ITS CHORUS OR LOAD ANOTHER TRACK");
        out->accent = 2;
        return 0;
    }
    drop = *b_target;
    if (b->pos_ms > drop.ms + 150u && !(loop && loop->valid)) {
        out->valid = 1;
        out->state = AN_ST_OFF;
        out->notice = AN_NOTICE_B_TARGET_PASSED;
        out->accent = 2;
        copy_str(out->line2, sizeof out->line2, "B TARGET PASSED - REPOSITION B OR CHOOSE ANOTHER");
        return 0;
    }

    return 1;
}

static double playback_rate(double rate)
{
    return rate > 0.2 && rate < 5.0 ? rate : 1.0;
}

static uint32_t bar_duration(double bpm)
{
    return bpm >= 20.0 && bpm <= 400.0 ? (uint32_t)(240000.0 / bpm) : 2000;
}

static plan_timing_t plan_timing(const an_deck_t *a, const an_deck_t *b, const an_loop_t *loop,
                                 uint32_t target_ms, an_cand_t drop, uint64_t now_ms)
{
    double arate = playback_rate(a->rate), brate = playback_rate(b->rate);
    uint32_t bar = bar_duration(a->bpm);
    double T = (double)now_ms + ((double)target_ms - (double)a->pos_ms) / arate;
    double T_out = T;
    if (loop && loop->valid) T_out = T - ((double)drop.ms - (double)loop->out_ms) / brate;
    double tol = (double)bar * 0.09;
    if (tol < 100.0) tol = 100.0;
    return (plan_timing_t){ brate, T, T_out, tol, bar, now_ms };
}

static void describe_plan(const plan_timing_t *t, uint32_t target_ms, const char *target_label, an_plan_t *out)
{
    /* line 1: the target and how far away it is */
    char bars[32];
    fmt_bars(bars, sizeof bars, t->target_wall - (double)t->now_ms, t->bar_ms);
    snprintf(out->line1, sizeof out->line1, "%s · %s", target_label && target_label[0] ? target_label : "TARGET", bars);
    out->target_ms = target_ms;
    copy_str(out->target, sizeof out->target, target_label);
}

static void released_plan(const plan_timing_t *t, const an_deck_t *b, an_cand_t drop, an_plan_t *out)
{
    double drop_in = (drop.ms > b->pos_ms) ? ((double)drop.ms - (double)b->pos_ms) / t->rate : 0.0;
    char s[64], bs[32];
    fmt_bars(bs, sizeof bs, drop_in, t->bar_ms);
    snprintf(s, sizeof s, "RELEASED · B TARGET IN %s", bs);
    set_action(out, AN_ST_RELEASED, 0, 4, s, drop_in, t->bar_ms, 0);
}

static int loop_timing_ready(const plan_timing_t *t, const an_deck_t *b, const an_loop_t *loop, an_cand_t drop, an_plan_t *out)
{
    int order = compare_timestamps(loop->out_ms, drop.ms, 1000, b->sample_rate, 1);
    if (order == AZ_TIME_INVALID || loop->out_ms <= loop->in_ms || loop->len_ms < 125) {
        out->notice = AN_NOTICE_LOOP_UNAVAILABLE;
        set_action(out, AN_ST_OFF, 0, 2, "LOOP TIMING UNAVAILABLE", 0, t->bar_ms, 0);
        return 0;
    }
    if (order == AZ_TIME_AFTER) {
        out->notice = AN_NOTICE_LOOP_AFTER_DROP;
        set_action(out, AN_ST_OFF, 0, 2, "LOOP OUT IS AFTER B TARGET — MOVE THE LOOP", 0, t->bar_ms, 0);
        return 0;
    }
    return 1;
}

static double closest_exit(const plan_timing_t *t, const an_deck_t *b, const an_loop_t *loop, double tloop)
{
    double d = (double)loop->out_ms - (double)b->pos_ms;
    if (d <= 0) {
        d = fmod(d, (double)loop->len_ms);
        if (d <= 0) d += loop->len_ms;
    }
    double next_out = (double)t->now_ms + d / t->rate;
    double best = next_out;
    double best_err = best - t->exit_wall;
    for (int k = 1; k < 64; k++) {
        double x = next_out + (double)k * tloop;
        double err = x - t->exit_wall;
        if (fabs(err) < fabs(best_err)) {
            best = x;
            best_err = err;
        }
        if (x - t->exit_wall > tloop * 2) break;
    }
    return best;
}

static void aligned_loop(const plan_timing_t *t, double best, double tloop, an_plan_t *out)
{
    double open_at = best - tloop;
    if ((double)t->now_ms >= open_at) {
        set_action(out, AN_ST_OPEN, 2, 4, "WINDOW OPEN — PRESS EXIT", 0, t->bar_ms, (uint64_t)best);
        out->window_open = 1;
        out->out_wall_ms = (uint64_t)best;
        out->loops = 0;
    } else {
        char s[64], bs[32];
        fmt_bars(bs, sizeof bs, open_at - (double)t->now_ms, t->bar_ms);
        snprintf(s, sizeof s, "RELEASE WINDOW IN %s", bs);
        set_action(out, AN_ST_WAIT, 0, 1, s, open_at - (double)t->now_ms, t->bar_ms, (uint64_t)best);
        out->out_wall_ms = (uint64_t)best;
    }
}

static void playing_loop(const plan_timing_t *t, const an_deck_t *b, const an_loop_t *loop, an_plan_t *out)
{
    double tloop = (double)loop->len_ms / t->rate;
    double best = closest_exit(t, b, loop, tloop);
    double best_err = best - t->exit_wall;
    if (fabs(best_err) <= t->tolerance) {
        aligned_loop(t, best, tloop, out);
    } else if (best_err > 0 && t->exit_wall < (double)t->now_ms) {
        char s[64], bs[32];
        double next_window = best - tloop;
        fmt_bars(bs, sizeof bs, next_window - (double)t->now_ms, t->bar_ms);
        snprintf(s, sizeof s, "MISSED — NEXT WINDOW IN %s", bs);
        set_action(out, AN_ST_MISSED, 0, 3, s, next_window - (double)t->now_ms, t->bar_ms, (uint64_t)best);
    } else {
        char s[64];
        if (best_err > 0)
            snprintf(s, sizeof s, "LOOP PHASE LATE BY %.0f ms — RESTART B", best_err);
        else
            snprintf(s, sizeof s, "LOOP PHASE EARLY BY %.0f ms — RESTART B", -best_err);
        set_action(out, AN_ST_OFF, 0, 2, s, 0, t->bar_ms, (uint64_t)best);
    }
}

static void paused_loop(const plan_timing_t *t, const an_deck_t *b, const an_loop_t *loop, an_plan_t *out)
{
    double tloop = (double)loop->len_ms / t->rate;
        /* B is paused: it is in (or will enter) a loop; find the next start window. */
    double d = (double)loop->out_ms - (double)b->pos_ms;
    if (d <= 0) {
        if ((double)b->pos_ms < (double)loop->in_ms)
            d = (double)loop->out_ms - (double)b->pos_ms; /* before the loop: play in */
        else {
            out->notice = AN_NOTICE_HEAD_PAST_LOOP;
            set_action(out, AN_ST_OFF, 0, 2, "B HEAD IS PAST THE LOOP — CUE AT ITS START", 0, t->bar_ms, 0);
            return;
        }
    }
    double base = t->exit_wall - d / t->rate;
    double earliest = (double)t->now_ms - AN_PLAY_GRACE_MS;
    if (base < earliest) {
        set_action(out, AN_ST_MISSED, 0, 3, "TOO LATE FOR THIS TARGET — PICK THE NEXT", 0, t->bar_ms, 0);
        return;
    }
    double extra = floor((base - earliest) / tloop);
    if (extra < 0) extra = 0;
    if (extra > 64) extra = 64;
    double start = base - extra * tloop;
    int loops = (int)extra + 1;
    char s[72], bs[32];
    fmt_bars(bs, sizeof bs, start - (double)t->now_ms, t->bar_ms);
    snprintf(s, sizeof s, "PLAY IN %s · %d LOOP%s", bs, loops, loops == 1 ? "" : "S");
    set_action(out, AN_ST_START, 1, 1, s, start - (double)t->now_ms, t->bar_ms, (uint64_t)fmax(0.0, start));
    out->loops = loops;
    return;
}

static void playing_unlooped(const plan_timing_t *t, const an_deck_t *b, an_cand_t drop, an_plan_t *out)
{
    if (drop.ms > b->pos_ms) {
        double drop_in = ((double)drop.ms - (double)b->pos_ms) / t->rate;
        double actual = (double)t->now_ms + drop_in;
        double err = actual - t->target_wall;
        char s[72], bs[32];
        fmt_bars(bs, sizeof bs, drop_in, t->bar_ms);
        if (fabs(err) <= t->tolerance)
            snprintf(s, sizeof s, "B ALREADY ALIGNED · TARGET IN %s", bs);
        else
            snprintf(s, sizeof s, "B PLAYING · TARGET IN %s — LOOP B TO ALIGN", bs);
        set_action(out, AN_ST_PLAYING, 0, fabs(err) <= t->tolerance ? 4 : 2, s, drop_in, t->bar_ms, 0);
        out->drop_aligned = fabs(err) <= t->tolerance;
    } else {
        set_action(out, AN_ST_PLAYING, 0, 2, "B IS PAST ITS DROP — PICK ANOTHER TARGET", 0, t->bar_ms, 0);
    }
    return;
}

static void paused_unlooped(const plan_timing_t *t, const an_deck_t *b, an_cand_t drop, an_plan_t *out)
{
    /* B paused, no loop: one exact play moment. */
    double start = t->target_wall - ((double)drop.ms - (double)b->pos_ms) / t->rate;
    if (start < (double)t->now_ms - AN_PLAY_GRACE_MS) {
        set_action(out, AN_ST_MISSED, 0, 3, "TOO LATE FOR THIS TARGET — PICK THE NEXT", 0, t->bar_ms, 0);
        return;
    }
    char s[72], bs[32];
    fmt_bars(bs, sizeof bs, start - (double)t->now_ms, t->bar_ms);
    snprintf(s, sizeof s, "PLAY IN %s · NO LOOP", bs);
    set_action(out, AN_ST_SINGLE, 1, 1, s, start - (double)t->now_ms, t->bar_ms, (uint64_t)fmax(0.0, start));
}

static void select_action(const plan_timing_t *t, const an_deck_t *b, const an_loop_t *loop, an_cand_t drop, an_plan_t *out)
{
    if (loop && loop->released) {
        released_plan(t, b, drop, out);
        return;
    }
    if (loop && loop->valid) {
        if (!loop_timing_ready(t, b, loop, drop, out)) return;
        if (b->playing) playing_loop(t, b, loop, out);
        else paused_loop(t, b, loop, out);
    } else if (b->playing) playing_unlooped(t, b, drop, out);
    else paused_unlooped(t, b, drop, out);
}

void an_plan_target(const an_deck_t *a, const an_deck_t *b, const an_loop_t *loop, uint32_t target_ms,
                    const char *target_label, const an_cand_t *b_target, uint64_t now_ms, an_plan_t *out)
{
    initialize_plan(b, loop, b_target, out);
    if (!loaded_plan(a, b, out) || !outgoing_target_ready(a, target_ms, out) ||
        !incoming_target_ready(b, loop, b_target, out)) return;
    plan_timing_t timing = plan_timing(a, b, loop, target_ms, *b_target, now_ms);
    describe_plan(&timing, target_ms, target_label, out);
    select_action(&timing, b, loop, *b_target, out);
}

static int b_hot_candidates(const an_analysis_t *b, an_cand_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < b->ncues && n < max; i++) {
        const an_cue_t *c = &b->cues[i];
        if (!c->slot) continue;
        out[n].ms = c->time_ms;
        snprintf(out[n++].label, sizeof out[0].label, "HC%u %.28s", c->slot, c->label);
    }
    return n;
}

static int b_phrase_candidates(const an_analysis_t *b, an_cand_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < b->nphrases && n < max; i++) {
        const an_phrase_t *phrase = &b->phrases[i];
        out[n].ms = phrase->start_ms;
        snprintf(out[n++].label, sizeof out[0].label, "%.30s START", an_category_name(phrase->category));
    }
    return n;
}

int an_b_candidates(const an_analysis_t *b, int hot_cues, an_cand_t *out, int max)
{
    if (!b || !out || max < 1) return 0;
    if (hot_cues) return b_hot_candidates(b, out, max);
    return b_phrase_candidates(b, out, max);
}
