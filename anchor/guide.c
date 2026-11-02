/* SPDX-License-Identifier: GPL-3.0-only
 *
 * guide.c: target selection and the transition plan.
 */
#include "guide.h"

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

/* Add a candidate, keeping the array sorted and dropping near-duplicates from another
 * source (a phrase end and a cue on the same beat, say). */
static void add_cand(an_cand_t *out, int *n, int max, uint32_t ms, const char *label, int chorus)
{
    if (ms == 0 || *n >= max) return;
    for (int i = 0; i < *n; i++) {
        int d = (int)ms - (int)out[i].ms;
        if (d < 0) d = -d;
        if (d <= 60) {
            if (chorus && !out[i].chorus) out[i].chorus = 1;
            return;
        }
    }
    int at = *n;
    while (at > 0 && out[at - 1].ms > ms) {
        out[at] = out[at - 1];
        at--;
    }
    out[at].ms = ms;
    copy_str(out[at].label, sizeof out[at].label, label);
    out[at].chorus = chorus;
    (*n)++;
}

static int grid_next(const an_anlz_t *a, uint32_t pos_ms, int bars, uint32_t *when, int maxn)
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

int an_phrase_category(const an_anlz_t *a, uint16_t kind)
{
    if (!a) return AN_CAT_OTHER;
    if (a->mood == 1) {
        switch (kind) {
        case 1: return AN_CAT_INTRO;
        case 2: return AN_CAT_BUILD;
        case 3: return AN_CAT_BREAK;
        case 5: return AN_CAT_CHORUS;
        case 6: return AN_CAT_OUTRO;
        default: return AN_CAT_OTHER;
        }
    }
    if (a->mood == 2 || a->mood == 3) {
        switch (kind) {
        case 1: return AN_CAT_INTRO;
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7: return AN_CAT_VERSE;
        case 8: return AN_CAT_BRIDGE;
        case 9: return AN_CAT_CHORUS;
        case 10: return AN_CAT_OUTRO;
        default: return AN_CAT_OTHER;
        }
    }
    switch (kind) {
    case 1: return AN_CAT_INTRO;
    case 5:
    case 9: return AN_CAT_CHORUS;
    case 6:
    case 10: return AN_CAT_OUTRO;
    case 8: return AN_CAT_BRIDGE;
    default: return AN_CAT_OTHER;
    }
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

int an_phrase_groups(const an_anlz_t *a, int consolidate, an_group_t *out, int max)
{
    int n = 0;
    if (!a || !a->nphrases || !out || max <= 0) return 0;
    for (int i = 0; i < a->nphrases; i++) {
        uint32_t s, e;
        if (an_anlz_phrase_start_ms(a, i, &s) != 0) continue;
        if (an_anlz_phrase_end_ms(a, i, &e) != 0) continue;
        int cat = an_phrase_category(a, a->phrases[i].kind);
        if (consolidate && n > 0 && out[n - 1].category == cat && out[n - 1].last == i - 1 &&
            a->phrases[i].start_beat <= a->phrases[i - 1].end_beat) {
            out[n - 1].last = i;
            out[n - 1].end_ms = e;
            continue;
        }
        if (n >= max) break;
        out[n].first = i;
        out[n].last = i;
        out[n].kind = a->phrases[i].kind;
        out[n].category = cat;
        out[n].start_ms = s;
        out[n].end_ms = e;
        n++;
    }
    return n;
}

static void label_group(char *dst, size_t n, const an_anlz_t *a, const an_group_t *g,
                        int start_point)
{
    snprintf(dst, n, "%s %s", an_anlz_kind_name(a, g->kind), start_point ? "START" : "END");
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

static int target_group(const an_anlz_t *a, const an_group_t *g, int n, int i,
                        int start_point, an_cand_t *out)
{
    if (i < 0 || i >= n) return -1;
    memset(out, 0, sizeof *out);
    out->ms = start_point ? g[i].start_ms : g[i].end_ms;
    label_group(out->label, sizeof out->label, a, &g[i], start_point);
    out->chorus = g[i].category == AN_CAT_CHORUS;
    return 0;
}

/* Grid boundary fallback, and the grid rules. */
static int target_grid(const an_anlz_t *a, uint32_t pos_ms, int bars, an_cand_t *out)
{
    uint32_t when[1];
    if (!a || grid_next(a, pos_ms, bars, when, 1) <= 0) return -1;
    memset(out, 0, sizeof *out);
    out->ms = when[0];
    snprintf(out->label, sizeof out->label, "%d-BAR", bars);
    return 0;
}

int an_auto_target(const an_anlz_t *a, uint32_t pos_ms, const an_target_opts_t *opts,
                   an_cand_t *out)
{
    memset(out, 0, sizeof *out);
    if (!a || !a->valid || !opts) return -1;

    if (opts->rule == AN_RULE_HOTCUE) {
        for (int i = 0; i < a->ncues; i++) {
            const an_cue_t *c = &a->cues[i];
            if (c->kind != AN_CUE_HOT) continue;
            if ((int64_t)c->time_ms <= (int64_t)pos_ms + 150) continue;
            if (!opts->hotcue_any && c->slot != (uint16_t)opts->hotcue_slot) continue;
            out->ms = c->time_ms;
            char slot[8];
            snprintf(slot, sizeof slot, "HC%u", c->slot);
            if (c->label[0] && strncmp(c->label, slot, strlen(slot)) != 0)
                snprintf(out->label, sizeof out->label, "%s %.24s", slot, c->label);
            else
                snprintf(out->label, sizeof out->label, "%s", c->label[0] ? c->label : slot);
            return 0;
        }
        return -1;
    }
    if (opts->rule == AN_RULE_GRID16 || opts->rule == AN_RULE_GRID32)
        return target_grid(a, pos_ms, opts->rule == AN_RULE_GRID32 ? 32 : 16, out);

    an_group_t g[256];
    int n = an_phrase_groups(a, opts->consolidate, g, 256);
    int start_point = opts->phrase_point == AN_POINT_START;
    if (n <= 0) return target_grid(a, pos_ms, 16, out);

    int at = group_at(g, n, pos_ms);
    int cat = opts->phrase_cat;

    if (cat >= 0) {
        for (int i = (at > 0 ? at : 0); i < n; i++) {
            if (g[i].category != cat) continue;
            uint32_t when = start_point ? g[i].start_ms : g[i].end_ms;
            if ((int64_t)when <= (int64_t)pos_ms + 150) continue;
            return target_group(a, g, n, i, start_point, out);
        }
        /* no phrase of the chosen kind ahead: fall through to the current group so the
         * guide still has a target, rather than going blank on an unusual track */
    }

    if (start_point) {
        int from = at + 1;
        if (from < 0) from = 0;
        for (int i = from; i < n; i++)
            if ((int64_t)g[i].start_ms > (int64_t)pos_ms + 150)
                return target_group(a, g, n, i, 1, out);
        for (int i = 0; i < n; i++)
            if ((int64_t)g[i].start_ms > (int64_t)pos_ms + 150)
                return target_group(a, g, n, i, 1, out);
        return -1;
    }

    int from = at;
    if (from < 0) from = 0;
    for (int i = from; i < n; i++)
        if ((int64_t)g[i].end_ms > (int64_t)pos_ms + 150)
            return target_group(a, g, n, i, 0, out);
    for (int i = 0; i < n; i++)
        if ((int64_t)g[i].end_ms > (int64_t)pos_ms + 150)
            return target_group(a, g, n, i, 0, out);
    return -1;
}

int an_candidates(const an_anlz_t *a, uint32_t pos_ms, const an_target_opts_t *opts,
                  an_cand_t *out, int max)
{
    int n = 0;
    if (!a || !a->valid || !out || max <= 0 || !opts) return 0;
    int start_point = opts->phrase_point == AN_POINT_START;

    an_group_t g[256];
    int ng = an_phrase_groups(a, opts->consolidate, g, 256);
    for (int i = 0; i < ng && n < max; i++) {
        uint32_t when = start_point ? g[i].start_ms : g[i].end_ms;
        if ((int64_t)when <= (int64_t)pos_ms + 250) continue;
        char label[40];
        label_group(label, sizeof label, a, &g[i], start_point);
        add_cand(out, &n, max, when, label, g[i].category == AN_CAT_CHORUS);
    }
    for (int i = 0; i < a->ncues && n < max; i++) {
        if ((int64_t)a->cues[i].time_ms <= (int64_t)pos_ms + 250) continue;
        char label[40];
        const an_cue_t *c = &a->cues[i];
        if (c->kind == AN_CUE_HOT) {
            char slot[8];
            snprintf(slot, sizeof slot, "HC%u", c->slot);
            if (c->label[0] && strncmp(c->label, slot, strlen(slot)) != 0)
                snprintf(label, sizeof label, "%s %.24s", slot, c->label);
            else
                snprintf(label, sizeof label, "%s", c->label[0] ? c->label : slot);
        } else {
            snprintf(label, sizeof label, "%s", c->label[0] ? c->label : "CUE");
        }
        add_cand(out, &n, max, c->time_ms, label, 0);
    }
    if (n < max && (ng == 0 || opts->rule == AN_RULE_GRID16 || opts->rule == AN_RULE_GRID32)) {
        int bars = opts->rule == AN_RULE_GRID32 ? 32 : 16;
        uint32_t when[4];
        int got = grid_next(a, pos_ms, bars, when, 3);
        for (int i = 0; i < got && n < max; i++) {
            char label[24];
            snprintf(label, sizeof label, "%d-BAR", bars);
            add_cand(out, &n, max, when[i], label, 0);
        }
    }
    return n;
}

int an_find_drop(const an_anlz_t *b, uint32_t pos_ms, an_cand_t *out)
{
    memset(out, 0, sizeof *out);
    if (!b || !b->valid) return -1;
    int fallback = -1;
    uint32_t fallback_ms = 0;
    if (b->has_phrases) {
        for (int i = 0; i < b->nphrases; i++) {
            uint32_t s;
            if (an_anlz_phrase_start_ms(b, i, &s) != 0) continue;
            if ((int64_t)s + 250 < (int64_t)pos_ms) continue;
            if (an_anlz_kind_is_chorus(b, b->phrases[i].kind)) {
                out->ms = s;
                snprintf(out->label, sizeof out->label, "%s", an_anlz_kind_name(b, b->phrases[i].kind));
                out->chorus = 1;
                return 0;
            }
            if (fallback < 0) {
                fallback = i;
                fallback_ms = s;
            }
        }
        if (fallback >= 0) {
            out->ms = fallback_ms;
            snprintf(out->label, sizeof out->label, "PHRASE");
            return 0;
        }
    }
    for (int i = 0; i < b->ncues; i++) {
        if (b->cues[i].time_ms + 200 < pos_ms) continue;
        out->ms = b->cues[i].time_ms;
        snprintf(out->label, sizeof out->label, "CUE %.32s", b->cues[i].label);
        return 0;
    }
    return -1;
}

uint32_t an_bar_ms(const an_deck_t *d, const an_anlz_t *a)
{
    double bpm = (d && d->bpm >= 20.0 && d->bpm <= 400.0) ? d->bpm : 0.0;
    if (bpm <= 0 && a) bpm = a->bpm;
    if (bpm < 20.0 || bpm > 400.0) bpm = 120.0;
    return (uint32_t)(4.0 * 60000.0 / bpm);
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

void an_plan_target(const an_deck_t *a, const an_anlz_t *aa, const an_deck_t *b,
             const an_anlz_t *ba, const an_loop_t *loop, uint32_t target_ms,
             const char *target_label, const an_cand_t *b_target, uint64_t now_ms, an_plan_t *out)
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
    uint32_t bar = an_bar_ms(a, aa);
    if (bar < 200) bar = 2000;

    if (!a || !b || !a->loaded || !b->loaded) {
        out->state = AN_ST_NEED_TRACKS;
        copy_str(out->line1, sizeof out->line1, "WAITING FOR TWO DECKS");
        copy_str(out->line2, sizeof out->line2, "LOAD A AND B IN THE 2-DECK VIEW");
        out->accent = 0;
        return;
    }
    if (!aa || !aa->valid || ((!ba || !ba->valid) && !b_target)) {
        out->notice = !aa || !aa->valid ? AN_NOTICE_ANALYSIS_A : AN_NOTICE_ANALYSIS_B;
        out->state = AN_ST_NO_ANALYSIS;
        copy_str(out->line1, sizeof out->line1, "NO PHRASE MAP");
        copy_str(out->line2, sizeof out->line2, "A USB TRACK WITH REKORDBOX ANALYSIS IS NEEDED");
        out->accent = 2;
        return;
    }
    if (!target_ms || (int64_t)target_ms <= (int64_t)a->pos_ms + 150) {
        out->state = AN_ST_NONE;
        copy_str(out->line1, sizeof out->line1, "NO TARGET AHEAD");
        copy_str(out->line2, sizeof out->line2, "TAP TO PICK ANOTHER TARGET");
        out->accent = 0;
        return;
    }

    an_cand_t drop;
    if (!b_target) {
        out->notice = AN_NOTICE_NO_DROP;
        out->state = AN_ST_NO_ANALYSIS;
        copy_str(out->line1, sizeof out->line1, "B HAS NO DROP AHEAD");
        copy_str(out->line2, sizeof out->line2, "CUE B BEFORE ITS CHORUS OR LOAD ANOTHER TRACK");
        out->accent = 2;
        return;
    }
    drop = *b_target;
    if (b->pos_ms > drop.ms + 150u && !(loop && loop->valid)) {
        out->valid = 1;
        out->state = AN_ST_OFF;
        out->notice = AN_NOTICE_B_TARGET_PASSED;
        out->accent = 2;
        copy_str(out->line2, sizeof out->line2, "B TARGET PASSED - REPOSITION B OR CHOOSE ANOTHER");
        return;
    }

    double arate = (a->rate > 0.2 && a->rate < 5.0) ? a->rate : 1.0;
    double brate = (b->rate > 0.2 && b->rate < 5.0) ? b->rate : 1.0;
    double T = (double)now_ms + ((double)target_ms - (double)a->pos_ms) / arate;
    double T_out = T;
    if (loop && loop->valid) T_out = T - ((double)drop.ms - (double)loop->out_ms) / brate;

    /* line 1: the target and how far away it is */
    char bars[32];
    fmt_bars(bars, sizeof bars, T - (double)now_ms, bar);
    snprintf(out->line1, sizeof out->line1, "%s · %s", target_label && target_label[0] ? target_label : "TARGET", bars);
    out->target_ms = target_ms;
    copy_str(out->target, sizeof out->target, target_label);

    double tol = (double)bar * 0.09;
    if (tol < 100.0) tol = 100.0;

    if (loop && loop->released) {
        double drop_in = (drop.ms > b->pos_ms) ? ((double)drop.ms - (double)b->pos_ms) / brate : 0.0;
        char s[64], bs[32];
        fmt_bars(bs, sizeof bs, drop_in, bar);
        snprintf(s, sizeof s, "RELEASED · B TARGET IN %s", bs);
        set_action(out, AN_ST_RELEASED, 0, 4, s, drop_in, bar, 0);
        return;
    }

    if (loop && loop->valid) {
        if (loop->out_ms <= loop->in_ms || loop->len_ms < 125) {
            out->notice = AN_NOTICE_LOOP_UNKNOWN;
            set_action(out, AN_ST_OFF, 0, 2, "LOOP UNKNOWN — START B TO DETECT IT", 0, bar, 0);
            return;
        }
        /* Native/grid conversions can differ by a millisecond at the same beat. */
        if (loop->out_ms > drop.ms + 2u) {
            out->notice = AN_NOTICE_LOOP_AFTER_DROP;
            set_action(out, AN_ST_OFF, 0, 2, "LOOP OUT IS AFTER B TARGET — MOVE THE LOOP", 0, bar, 0);
            return;
        }
        double tloop = (double)loop->len_ms / brate;
        if (b->playing) {
            double d = (double)loop->out_ms - (double)b->pos_ms;
            if (d <= 0) { d = fmod(d, (double)loop->len_ms); if (d <= 0) d += loop->len_ms; }
            double next_out = (double)now_ms + d / brate;
            double best = next_out;
            double best_err = best - T_out;
            for (int k = 1; k < 64; k++) {
                double x = next_out + (double)k * tloop;
                double err = x - T_out;
                if (fabs(err) < fabs(best_err)) {
                    best = x;
                    best_err = err;
                }
                if (x - T_out > tloop * 2) break;
            }
            if (fabs(best_err) <= tol) {
                double open_at = best - tloop;
                if ((double)now_ms >= open_at) {
                    set_action(out, AN_ST_OPEN, 2, 4, "WINDOW OPEN — PRESS EXIT", 0, bar, (uint64_t)best);
                    out->window_open = 1;
                    out->out_wall_ms = (uint64_t)best;
                    out->loops = 0;
                } else {
                    char s[64], bs[32];
                    fmt_bars(bs, sizeof bs, open_at - (double)now_ms, bar);
                    snprintf(s, sizeof s, "RELEASE WINDOW IN %s", bs);
                    set_action(out, AN_ST_WAIT, 0, 1, s, open_at - (double)now_ms, bar, (uint64_t)best);
                    out->out_wall_ms = (uint64_t)best;
                }
            } else if (best_err > 0 && T_out < (double)now_ms) {
                char s[64], bs[32];
                double next_window = best - tloop;
                fmt_bars(bs, sizeof bs, next_window - (double)now_ms, bar);
                snprintf(s, sizeof s, "MISSED — NEXT WINDOW IN %s", bs);
                set_action(out, AN_ST_MISSED, 0, 3, s, next_window - (double)now_ms, bar, (uint64_t)best);
            } else {
                char s[64];
                if (best_err > 0)
                    snprintf(s, sizeof s, "LOOP PHASE LATE BY %.0f ms — RESTART B", best_err);
                else
                    snprintf(s, sizeof s, "LOOP PHASE EARLY BY %.0f ms — RESTART B", -best_err);
                set_action(out, AN_ST_OFF, 0, 2, s, 0, bar, (uint64_t)best);
            }
            return;
        }

        /* B is paused: it is in (or will enter) a loop; find the next start window. */
        double d = (double)loop->out_ms - (double)b->pos_ms;
        if (d <= 0) {
            if ((double)b->pos_ms < (double)loop->in_ms)
                d = (double)loop->out_ms - (double)b->pos_ms; /* before the loop: play in */
            else {
                out->notice = AN_NOTICE_HEAD_PAST_LOOP;
                set_action(out, AN_ST_OFF, 0, 2, "B HEAD IS PAST THE LOOP — CUE AT ITS START", 0, bar, 0);
                return;
            }
        }
        double base = T_out - d / brate;
        double earliest = (double)now_ms - AN_PLAY_GRACE_MS;
        if (base < earliest) {
            set_action(out, AN_ST_MISSED, 0, 3, "TOO LATE FOR THIS TARGET — PICK THE NEXT", 0, bar, 0);
            return;
        }
        double extra = floor((base - earliest) / tloop);
        if (extra < 0) extra = 0;
        if (extra > 64) extra = 64;
        double start = base - extra * tloop;
        int loops = (int)extra + 1;
        char s[72], bs[32];
        fmt_bars(bs, sizeof bs, start - (double)now_ms, bar);
        snprintf(s, sizeof s, "PLAY IN %s · %d LOOP%s", bs, loops, loops == 1 ? "" : "S");
        set_action(out, AN_ST_START, 1, 1, s, start - (double)now_ms, bar, (uint64_t)fmax(0.0, start));
        out->loops = loops;
        return;
    }

    if (b->playing) {
        if (drop.ms > b->pos_ms) {
            double drop_in = ((double)drop.ms - (double)b->pos_ms) / brate;
            double actual = (double)now_ms + drop_in;
            double err = actual - T;
            char s[72], bs[32];
            fmt_bars(bs, sizeof bs, drop_in, bar);
            if (fabs(err) <= tol)
                snprintf(s, sizeof s, "B ALREADY ALIGNED · TARGET IN %s", bs);
            else
                snprintf(s, sizeof s, "B PLAYING · TARGET IN %s — LOOP B TO ALIGN", bs);
            set_action(out, AN_ST_PLAYING, 0, fabs(err) <= tol ? 4 : 2, s, drop_in, bar, 0);
            out->drop_aligned = fabs(err) <= tol;
        } else {
            set_action(out, AN_ST_PLAYING, 0, 2, "B IS PAST ITS DROP — PICK ANOTHER TARGET", 0, bar, 0);
        }
        return;
    }

    /* B paused, no loop: one exact play moment. */
    double start = T - ((double)drop.ms - (double)b->pos_ms) / brate;
    if (start < (double)now_ms - AN_PLAY_GRACE_MS) {
        set_action(out, AN_ST_MISSED, 0, 3, "TOO LATE FOR THIS TARGET — PICK THE NEXT", 0, bar, 0);
        return;
    }
    char s[72], bs[32];
    fmt_bars(bs, sizeof bs, start - (double)now_ms, bar);
    snprintf(s, sizeof s, "PLAY IN %s · NO LOOP", bs);
    set_action(out, AN_ST_SINGLE, 1, 1, s, start - (double)now_ms, bar, (uint64_t)fmax(0.0, start));
}

/* Compatibility helper for callers that deliberately request an automatic destination. */
void an_plan(const an_deck_t *a, const an_anlz_t *aa, const an_deck_t *b,
             const an_anlz_t *ba, const an_loop_t *loop, uint32_t target_ms,
             const char *target_label, uint64_t now_ms, an_plan_t *out)
{
    an_cand_t destination;
    const an_cand_t *selected = b && an_find_drop(ba, b->pos_ms, &destination) == 0 ? &destination : NULL;
    an_plan_target(a, aa, b, ba, loop, target_ms, target_label, selected, now_ms, out);
}

int an_suggest_phrase(const an_anlz_t *b, uint32_t pos_ms, an_cand_t *out)
{
    if (!b || !b->valid || !b->has_phrases) return -1;
    an_anlz_t phrases = *b;
    phrases.ncues = 0;
    return an_find_drop(&phrases, pos_ms, out);
}

int an_b_candidates(const an_anlz_t *b, int hot_cues, an_cand_t *out, int max)
{
    int n = 0;
    if (!b || !b->valid || !out || max < 1) return 0;
    if (hot_cues) {
        for (int i = 0; i < b->ncues && n < max; i++) {
            const an_cue_t *c = &b->cues[i];
            if (c->kind != AN_CUE_HOT) continue;
            out[n].ms = c->time_ms;
            out[n].chorus = 0;
            snprintf(out[n++].label, sizeof out[0].label, "HC%u %.28s", c->slot, c->label);
        }
    } else {
        for (int i = 0; i < b->nphrases && n < max; i++) {
            uint32_t ms;
            if (an_anlz_phrase_start_ms(b, i, &ms) != 0) continue;
            out[n].ms = ms;
            out[n].chorus = an_anlz_kind_is_chorus(b, b->phrases[i].kind);
            snprintf(out[n++].label, sizeof out[0].label, "%.30s START", an_anlz_kind_name(b, b->phrases[i].kind));
        }
    }
    return n;
}
