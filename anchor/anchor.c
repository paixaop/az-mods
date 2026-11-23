/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Anchor follows the SDK's loaded-deck events and paints a read-only PLAY/EXIT
 * guide for aligning a point on B with a phrase or cue on A.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "azmod-timestamps.h"
#include "deck.h"
#include "display.h"
#include "mod.h"
#include "settings.h"

#define MOD_ID ANCHOR_MOD_ID
#define MAX_CAND (AN_MAX_PHRASES + AN_MAX_HOT_CUES + AN_MAX_MEMORY_CUES + 3)

static const char *const TAGS[] = { "any", NULL };
static const az_core_v1 *C;
static az_surface *strip;
static az_surface *sidebar_tab;
static az_deck_subscription subscription;

static an_deck_status_t decks[AN_DECKS];
static int cur_a = -1, cur_b = -1;
static int cur_layout;
static int tab_shown, tab_open = -1;
static int cur_deck_setting = -1;
static an_target_opts_t cur_opts;
static an_plan_t plan;
static char shown_line2[96];
static an_display_t shown_display;
static int shown_display_valid, strip_shown;

static int sel_open, sel_page, sel_n, sel_per = 4;
static int sel_deck = -1, sel_b, sel_kind, sel_pending;
static an_cand_t sel_cand[MAX_CAND];
static uint32_t latched_ms;
static char latched_label[40];

static az_rect chip_r[4], arrow_l_r, arrow_r_r, close_r;
static az_rect kind_r[3], target_controls[2];
static int layout_ok, controls_ok;

static struct {
    int valid;
    an_cand_t target;
} b_targets[AN_DECKS];

static void update_guide(uint64_t now);

static uint64_t mono_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t accent_rgb(int accent)
{
    switch (accent) {
    case 4: return 0x34c759; /* green: aligned / window open */
    case 3: return 0xff453a; /* red: missed */
    case 2: return 0xffb340; /* amber: warning */
    case 1: return C ? C->color(AZ_COL_TEXT) : 0xffffff;
    default: return C ? C->color(AZ_COL_TEXT_DIM) : 0x888888;
    }
}

static int selected_layout(void)
{
    return settings_get(SET_DISPLAY) + ANCHOR_LAYOUT_RIBBON;
}

static void close_selector(void)
{
    sel_open = layout_ok = controls_ok = 0;
}

static void invalidate_deck(int d)
{
    memset(&b_targets[d], 0, sizeof b_targets[d]);
    if (d != cur_a && d != cur_b) return;
    cur_a = cur_b = -1;
    latched_ms = 0;
    close_selector();
    shown_display_valid = 0;
}

static void notice(int state, int reason, int accent, const char *detail)
{
    plan = (an_plan_t){ .valid = 1, .state = state, .notice = reason, .accent = accent };
    snprintf(plan.line2, sizeof plan.line2, "%s", detail);
}

static int bpm_valid(double bpm)
{
    return bpm >= 20 && bpm <= 400;
}

static int rate_valid(double rate)
{
    return rate > .2 && rate < 5;
}

static int derive_rate(int playing, double rate, double bpm, double native)
{
    return (!playing || !rate_valid(rate)) && bpm_valid(bpm) && bpm_valid(native);
}

static int deck_tempo(const an_deck_status_t *deck, int playing, double *bpm_out, double *rate_out)
{
    double native = an_grid_bpm_at(&deck->analysis, deck->pos_ms);
    if (!bpm_valid(native)) native = deck->original_bpm;
    double bpm = deck->bpm, rate = deck->rate;
    if (derive_rate(playing, rate, bpm, native))
        rate = bpm / native;
    if (!rate_valid(rate)) return 0;
    if (!bpm_valid(bpm)) bpm = native * rate;
    if (!bpm_valid(bpm)) return 0;

    *bpm_out = bpm;
    *rate_out = rate;
    return 1;
}

static int project_head(const an_deck_status_t *deck, uint64_t now, double rate, uint32_t *pos)
{
        /* Bring independently delivered heads to a common monotonic time. A
         * stalled or reversed head cannot support a forward countdown. */
    uint64_t observed = deck->head_ns / 1000000u;
    if (deck->direction != AZ_STATUS_DIRECTION_FORWARD || !deck->head_ns ||
        now < observed || now - observed > 150) return 0;
    uint32_t advance = (uint32_t)llround(rate * (now - observed));
    if (deck->loop.valid && *pos + advance >= deck->loop.out_ms)
        return 0; /* Wait for the SDK's wrap instead of projecting past OUT. */
    *pos += advance;
    return 1;
}

static int deck_for_plan(const an_deck_status_t *deck, uint64_t now, an_deck_t *out)
{
    if (!deck->position_valid) return 0;
    int playing = deck->playback == AZ_PLAYBACK_PLAYING;
    double bpm, rate;
    if (!deck_tempo(deck, playing, &bpm, &rate)) return 0;

    uint32_t pos = deck->pos_ms;
    if (playing) {
        if (!project_head(deck, now, rate, &pos)) return 0;
    }
    *out = (an_deck_t){ .loaded = 1, .playing = playing, .pos_ms = pos, .bpm = bpm, .rate = rate, .sample_rate = deck->sample_rate };
    return 1;
}

static int prepare_incoming_target(const an_deck_status_t *incoming, const an_analysis_t *ba, int b, uint32_t pos_ms)
{
    int cues_pending = an_availability_pending(incoming->hot_cues) ||
                       an_availability_pending(incoming->memory_cues);
    if (!b_targets[b].valid && !incoming->phrases_pending) {
        an_analysis_t available = *ba;
        if (cues_pending) available.ncues = 0;
        b_targets[b].valid = an_find_drop(&available, pos_ms, &b_targets[b].target) == 0;
    }

    return cues_pending;
}

static int outgoing_rule(const an_deck_status_t *outgoing, uint32_t pos_ms, an_target_opts_t *use)
{
    if (latched_ms && (uint64_t)latched_ms <= (uint64_t)pos_ms + 150) latched_ms = 0;
    if (!latched_ms && use->rule == AN_RULE_PHRASE && !outgoing->analysis.nphrases) {
        if (outgoing->phrases_pending) {
            notice(AN_ST_NO_ANALYSIS, AN_NOTICE_READING, 0, "Waiting for the outgoing phrase map");
            return 0;
        }
        use->rule = AN_RULE_HOTCUE;
    }

    return 1;
}

static void outgoing_cue_notice(int availability, int rule)
{
    if (an_availability_pending(availability))
        notice(AN_ST_NO_ANALYSIS, AN_NOTICE_READING, 0, "Waiting for outgoing Hot Cues");
    else if (availability == AZ_AVAIL_EMPTY)
        notice(AN_ST_NO_ANALYSIS, rule == AN_RULE_HOTCUE ? AN_NOTICE_NO_CUES : AN_NOTICE_ANALYSIS_A,
               2, "Choose a phrase or grid target");
    else
        notice(AN_ST_NO_ANALYSIS, AN_NOTICE_CUES_UNAVAILABLE, 2, "Choose a phrase or grid target");
}

static int outgoing_target(const an_deck_status_t *outgoing, uint32_t pos_ms, const an_target_opts_t *opts, const an_target_opts_t *use, an_cand_t *target)
{
    if (latched_ms) {
        *target = (an_cand_t){ .ms = latched_ms };
        snprintf(target->label, sizeof target->label, "%s", latched_label);
    } else if (use->rule == AN_RULE_HOTCUE && outgoing->hot_cues != AZ_AVAIL_READY) {
        outgoing_cue_notice(outgoing->hot_cues, opts->rule);
        return 0;
    } else if (use->rule >= AN_RULE_GRID16 && !outgoing->analysis.nbeats) {
        notice(AN_ST_NO_ANALYSIS, outgoing->grid_pending ? AN_NOTICE_READING : AN_NOTICE_ANALYSIS_A,
               2, "Waiting for the outgoing beat grid");
        return 0;
    } else if (an_auto_target(&outgoing->analysis, pos_ms, use, target) != 0) {
        notice(AN_ST_NONE, AN_NOTICE_NONE, 0, "Choose an outgoing target ahead of the playhead");
        return 0;
    }

    return 1;
}

static int incoming_drop_ready(const an_deck_status_t *incoming, int b, int cues_pending)
{
    if (!b_targets[b].valid) {
        int reason = AN_NOTICE_NO_DROP;
        if (incoming->phrases_pending || cues_pending) reason = AN_NOTICE_READING;
        else if (!incoming->analysis.nphrases && !incoming->analysis.ncues) reason = AN_NOTICE_ANALYSIS_B;
        notice(AN_ST_NO_ANALYSIS, reason, 2, "Choose a phrase, Hot Cue or Set here on B");
        return 0;
    }
    return 1;
}

static int incoming_loop_ready(const an_deck_status_t *incoming)
{
    if (incoming->loop_availability != AZ_AVAIL_READY) {
        int pending = an_availability_pending(incoming->loop_availability);
        notice(AN_ST_OFF, pending ? AN_NOTICE_LOOP_UNKNOWN : AN_NOTICE_LOOP_UNAVAILABLE,
               pending ? 0 : 2, pending ? "Waiting for the working loop" : "Working loop state is unavailable");
        return 0;
    }

    return 1;
}

static void check_tempo_match(double outgoing, double incoming)
{
    if (fabs(outgoing - incoming) > .15 &&
        (plan.state == AN_ST_SINGLE || plan.state == AN_ST_START ||
         plan.state == AN_ST_WAIT || plan.state == AN_ST_OPEN)) {
        plan.tempo_mismatch = 1;
        plan.accent = 2;
    }
}

static void compute_plan(uint64_t now, int a, int b, const an_target_opts_t *opts)
{
    const an_deck_status_t *outgoing = &decks[a], *incoming = &decks[b];
    const an_analysis_t *ba = &incoming->analysis;
    an_deck_t A, B;
    if (!deck_for_plan(outgoing, now, &A) || !deck_for_plan(incoming, now, &B)) {
        notice(AN_ST_OFF, AN_NOTICE_DECK_STATE, 0, "Waiting for playhead and tempo");
        return;
    }

    int cues_pending = prepare_incoming_target(incoming, ba, b, B.pos_ms);

    an_target_opts_t use = *opts;
    if (!outgoing_rule(outgoing, A.pos_ms, &use)) return;

    an_cand_t target;
    if (!outgoing_target(outgoing, A.pos_ms, opts, &use, &target)) return;

    if (!incoming_drop_ready(incoming, b, cues_pending)) return;
    if (!incoming_loop_ready(incoming)) return;

    an_plan_target(&A, &B, &incoming->loop, target.ms, target.label, &b_targets[b].target, now, &plan);
    check_tempo_match(A.bpm, B.bpm);
}

/* ------------------------------------------------------------------------------------------
 * drawing
 */
static int text_w(const char *s, int px)
{
    if (!C || !s || !s[0]) return 0;
    int w = C->text_width(s, px);
    return w > 0 ? w : (int)strlen(s) * px * 3 / 5;
}

/* Fit s into maxw, cutting at a character and ending with "...". Never returns text wider
 * than maxw, so a long state or a long title cannot run into the next element. */
static void fit_text(char *out, size_t n, const char *s, int px, int maxw)
{
    if (!n) return;
    out[0] = 0;
    if (!s || maxw <= px) return;
    if (text_w(s, px) <= maxw) {
        snprintf(out, n, "%s", s);
        return;
    }
    size_t len = strlen(s);
    if (len > n - 1) len = n - 1;
    while (len > 0) {
        char tmp[160];
        snprintf(tmp, sizeof tmp, "%.*s...", (int)len, s);
        if (text_w(tmp, px) <= maxw) {
            snprintf(out, n, "%s", tmp);
            return;
        }
        len--;
    }
    snprintf(out, n, "...");
}

static void draw_fit(az_draw *d, int x, int y, int w, const char *s, int px, uint32_t rgb)
{
    char buf[160];
    if (!C || w < px || !s || px < 8) return;
    fit_text(buf, sizeof buf, s, px, w);
    if (buf[0]) C->draw_text(d, x, y, buf, px, rgb);
}

static void draw_plan(az_draw *d, az_rect a)
{
    an_display_t m;
    an_display_model(&plan, cur_a, cur_b, &m);
    an_display_draw(C, d, a, cur_layout, &m);
    an_display_controls(a, cur_layout, target_controls);
    controls_ok = target_controls[0].w > 0;
}

static void draw_chip(az_draw *d, az_rect r, const char *label, int selected)
{
    uint32_t border = selected ? accent_rgb(4) : C->color(AZ_COL_TEXT_DIM);
    C->fill_rect(d, r.x, r.y, r.w, r.h, C->color(AZ_COL_BG));
    C->draw_rect(d, r.x, r.y, r.w, r.h, selected ? 2 : 1, border);
    int px = r.h >= 32 ? 13 : 11;
    if (r.w < 30 || r.h < 16) return;
    while (px > 9 && text_w(label, px) > r.w - 10) px--;
    int tw = text_w(label, px);
    int x = r.x + 5;
    if (tw < r.w - 10) x = r.x + (r.w - tw) / 2;
    int y = r.y + (r.h - px * 3 / 2) / 2;
    draw_fit(d, x, y, r.w - 10, label, px, C->color(AZ_COL_TEXT));
}

static int candidate_selected(uint32_t ms)
{
    if (sel_deck < 0 || (sel_b ? !b_targets[sel_deck].valid : !latched_ms)) return 0;
    uint32_t selected = sel_b ? b_targets[sel_deck].target.ms : latched_ms;
    int order = compare_timestamps(ms, selected, 1000, decks[sel_deck].sample_rate, 1);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL;
}

static int selector_pages(void)
{
    int pages = (sel_n + sel_per - 1) / sel_per;
    if (pages < 1) pages = 1;
    if (sel_page >= pages) sel_page = pages - 1;
    if (sel_page < 0) sel_page = 0;
    return pages;
}

static int sidebar_candidate(az_draw *d, az_rect a, int i)
{
    char hint[64];
    int idx = sel_page * sel_per + i;
    if (idx >= sel_n) return 0;
    az_rect r = { a.x + 8, a.y + 38 + i * 49, a.w - 16, 44 };
    chip_r[i] = r;
    int selected = candidate_selected(sel_cand[idx].ms);
    C->draw_rect(d, r.x, r.y, r.w, r.h, selected ? 2 : 1,
                 C->color(selected ? AZ_COL_HI : AZ_COL_TEXT_DIM));
    draw_fit(d, r.x + 5, r.y + 4, r.w - 10, sel_cand[idx].label, 11, C->color(AZ_COL_TEXT));
    unsigned sec = sel_cand[idx].ms / 1000u;
    snprintf(hint, sizeof hint, "%u:%02u", sec / 60u, sec % 60u);
    draw_fit(d, r.x + 5, r.y + 23, r.w - 10, hint, 10, C->color(AZ_COL_TEXT_DIM));
    return 1;
}

static void draw_sidebar_selector(az_draw *d, az_rect a)
{
    sel_per = 4;
    int pages = selector_pages();
    char hint[64];
    snprintf(hint, sizeof hint, "%c: DECK %d  %d/%d", sel_b ? 'B' : 'A', sel_deck + 1, sel_page + 1, pages);
    draw_fit(d, a.x + 8, a.y + 8, a.w - 16, hint, 11, C->color(AZ_COL_TEXT));
    for (int i = 0; i < sel_per; i++)
        if (!sidebar_candidate(d, a, i)) break;
    if (!sel_n) draw_fit(d, a.x + 8, a.y + 80, a.w - 16,
                         sel_pending ? "READING TARGETS" : "NO TARGETS", 12, C->color(AZ_COL_TEXT));
    snprintf(hint, sizeof hint, "%d loop pass%s", plan.loops, plan.loops == 1 ? "" : "es");
    draw_fit(d, a.x + 8, a.y + a.h - 62, a.w - 16, hint, 10, C->color(AZ_COL_TEXT_DIM));
    int bw = (a.w - 24) / 3;
    int by = a.y + a.h - 40;
    arrow_l_r = (az_rect){ a.x + 8, by, bw, 32 };
    arrow_r_r = (az_rect){ a.x + 12 + bw, by, bw, 32 };
    close_r = (az_rect){ a.x + 16 + 2 * bw, by, bw, 32 };
    draw_chip(d, arrow_l_r, "<", 0);
    draw_chip(d, arrow_r_r, ">", 0);
    draw_chip(d, close_r, "X", 0);
    layout_ok = 1;
}

static az_rect selector_geometry(az_rect a)
{
    int pad = 6;
    int hint_h = sel_b ? 0 : 12;

    int arrow_px = 16;
    int close_w = text_w("X", arrow_px) + 16;
    int arrow_w = text_w("<", arrow_px) + 14;
    int chips_x = a.x + pad + arrow_w;
    int chips_w = a.w - 2 * pad - close_w - 2 * arrow_w;
    int cy = a.y + hint_h + 2;
    int ch = a.y + a.h - cy - 2;
    int max_chip_h = a.h >= 90 ? 44 : 34;
    if (ch > max_chip_h) ch = max_chip_h;
    if (ch < 12) ch = 12;

    arrow_l_r = (az_rect){ a.x + pad, cy, arrow_w - 2, ch };
    arrow_r_r = (az_rect){ a.x + a.w - pad - close_w - arrow_w, cy, arrow_w - 2, ch };
    close_r = (az_rect){ a.x + a.w - pad - close_w, cy, close_w, ch };

    return (az_rect){ chips_x, cy, chips_w, ch };
}

static int selector_page_size(int chips_w, int gap)
{
    /* How many chips fit is measured, not fixed: a long label ("BRIDGE END · 12:34")
     * shrinks the page, so no chip text is ever drawn over its neighbour. */
    int widest = 30;
    for (int i = 0; i < sel_n; i++) {
        char disp[64];
        unsigned sec = sel_cand[i].ms / 1000u;
        snprintf(disp, sizeof disp, "%s · %u:%02u", sel_cand[i].label, sec / 60u, sec % 60u);
        int w = text_w(disp, 11) + 12;
        if (w > widest) widest = w;
    }
    int per = chips_w / (widest + gap);
    if (per > 4) per = 4;
    if (per < 1) per = 1;
    sel_per = per;
    return per;
}

static void selector_hint(az_draw *d, az_rect a, int pages)
{
    const int pad = 6, hint_px = 9;
    if (!sel_b) {
        char hint[96];
        if (a.w >= 620)
            snprintf(hint, sizeof hint, "TAP A TARGET  ·  < > PAGES  ·  X CLOSE  ·  %d/%d",
                     sel_page + 1, pages);
        else if (a.w >= 380)
            snprintf(hint, sizeof hint, "TAP A TARGET · %d/%d", sel_page + 1, pages);
        else
            snprintf(hint, sizeof hint, "TARGETS %d/%d", sel_page + 1, pages);
        draw_fit(d, a.x + pad, a.y + 1, a.w - 2 * pad, hint, hint_px,
                 C->color(AZ_COL_TEXT_DIM));
    }
}

static void selector_chips(az_draw *d, az_rect chips)
{
    int chips_x = chips.x, chips_w = chips.w, cy = chips.y, ch = chips.h;
    int per = sel_per, gap = 4;
    int cw = (chips_w - (per - 1) * gap) / per;
    if (cw < 20) cw = 20;

    for (int i = 0; i < per; i++) {
        int idx = sel_page * per + i;
        az_rect r = { chips_x + i * (cw + gap), cy, cw, ch };
        chip_r[i] = r;
        if (idx < sel_n) {
            char disp[64];
            unsigned sec = sel_cand[idx].ms / 1000u;
            snprintf(disp, sizeof disp, "%s · %u:%02u", sel_cand[idx].label, sec / 60u, sec % 60u);
            draw_chip(d, r, disp, candidate_selected(sel_cand[idx].ms));
        } else {
            draw_chip(d, r, "", 0);
        }
    }
}

static void selector_detail(az_draw *d, az_rect a, az_rect chips)
{
    int chips_x = chips.x, chips_w = chips.w, cy = chips.y, ch = chips.h, pad = 6;
    if (sel_n == 0)
        draw_fit(d, chips_x, cy + (ch - 12 * 3 / 2) / 2, chips_w,
                 sel_pending ? "READING TARGETS" : sel_b ? "NO MARKERS - USE SET HERE"
                                                         : "NO TARGETS AHEAD - ANALYSE TRACK A",
                 12, C->color(AZ_COL_TEXT_DIM));
    if (a.h >= 90) {
        char detail[128];
        snprintf(detail, sizeof detail, "%s | %d loop pass%s", plan.line2, plan.loops,
                 plan.loops == 1 ? "" : "es");
        draw_fit(d, a.x + pad, a.y + a.h - 22, a.w - 2 * pad, detail, 11, C->color(AZ_COL_TEXT_DIM));
    }
}

static void draw_compact_selector(az_draw *d, az_rect a)
{
    az_rect chips = selector_geometry(a);
    int chips_w = chips.w, cy = chips.y, ch = chips.h;
    const int gap = 4, arrow_px = 16;

    selector_page_size(chips_w, gap);
    int pages = selector_pages();
    selector_hint(d, a, pages);
    selector_chips(d, chips);
    C->draw_text(d, arrow_l_r.x + 4, cy + (ch - arrow_px * 3 / 2) / 2, "<", arrow_px,
                 C->color(AZ_COL_TEXT_DIM));
    C->draw_text(d, arrow_r_r.x + 4, cy + (ch - arrow_px * 3 / 2) / 2, ">", arrow_px,
                 C->color(AZ_COL_TEXT_DIM));
    C->draw_text(d, close_r.x + 5, cy + (ch - arrow_px * 3 / 2) / 2, "X", arrow_px,
                 C->color(AZ_COL_TEXT_DIM));

    selector_detail(d, a, chips);
    layout_ok = 1;
}

static void draw_selector_list(az_draw *d, az_rect a)
{
    /* Invalidate old hit rectangles before every layout, including tiny fallbacks. */
    memset(chip_r, 0, sizeof chip_r);
    memset(&arrow_l_r, 0, sizeof arrow_l_r);
    memset(&arrow_r_r, 0, sizeof arrow_r_r);
    memset(&close_r, 0, sizeof close_r);
    layout_ok = 0;
    C->fill_rect(d, a.x, a.y, a.w, a.h, C->color(AZ_COL_BG));
    C->draw_rect(d, a.x, a.y, a.w, a.h, 1, C->color(AZ_COL_TEXT_DIM));
    if (cur_layout == ANCHOR_LAYOUT_SIDEBAR && a.w >= 140 && a.h >= 260) {
        draw_sidebar_selector(d, a);
        return;
    }
    if (a.w < 260 || a.h < (sel_b ? 20 : 42)) {
        draw_fit(d, a.x + 8, a.y + (a.h - 12 * 3 / 2) / 2, a.w - 16, "TAP TARGET", 12,
                 C->color(AZ_COL_TEXT_DIM));
        layout_ok = 1;
        return;
    }

    draw_compact_selector(d, a);
}

static void draw_selector(az_draw *d, az_rect a)
{
    memset(kind_r, 0, sizeof kind_r);
    controls_ok = 0;
    if (!sel_b || a.w < 140 || a.h < 50) {
        draw_selector_list(d, a);
        return;
    }
    C->fill_rect(d, a.x, a.y, a.w, a.h, C->color(AZ_COL_BG));
    int width = cur_layout == ANCHOR_LAYOUT_SIDEBAR ? (a.w - 12) / 3 : 108;
    int height = a.h >= 100 ? 32 : 24;
    const char *names[] = { "PHRASE", "HOT CUE", "SET HERE" };
    for (int i = 0; i < 3; i++) {
        kind_r[i] = (az_rect){ a.x + 4 + i * (width + 2), a.y + 2, width, height };
        draw_chip(d, kind_r[i], names[i], i == sel_kind);
    }
    if (a.w > 420) draw_fit(d, a.x + 340, a.y + 4, a.w - 348,
                            "B TARGET / DECK DESTINATION", 11, C->color(AZ_COL_TEXT_DIM));
    a.y += height + 4;
    a.h -= height + 4;
    draw_selector_list(d, a);
}

static void paint(az_draw *d, az_rect a, void *user)
{
    (void)d;
    (void)user;
    if (!C || a.w < 80 || a.h < 24) return;
    if (sel_open)
        draw_selector(d, a);
    else
        draw_plan(d, a);
}

static void tab_paint(az_draw *d, az_rect a, void *user)
{
    (void)user;
    an_sidebar_tab_draw(C, d, a, settings_get(SET_SHOW));
}

static int tab_input(const az_event *ev, void *user)
{
    (void)user;
    az_rect r = an_sidebar_tab_rect(settings_get(SET_SHOW));
    if (!ev || ev->kind != AZ_EV_TAP || ev->x < r.x || ev->y < r.y ||
        ev->x >= r.x + r.w || ev->y >= r.y + r.h) return 0;
    settings_set(SET_SHOW, !settings_get(SET_SHOW));
    return 1;
}

/* ------------------------------------------------------------------------------------------
 * input
 */
static int in_rect(const az_rect *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static void refresh_selector(void)
{
    sel_n = sel_pending = layout_ok = controls_ok = 0;
    const an_deck_status_t *deck = &decks[sel_deck];
    if (sel_b) {
        sel_n = an_b_candidates(&deck->analysis, sel_kind == 1, sel_cand, MAX_CAND);
        sel_pending = sel_kind == 1 ? an_availability_pending(deck->hot_cues) : deck->phrases_pending;
    } else if (deck->position_valid) {
        sel_n = an_candidates(&deck->analysis, deck->pos_ms, deck->sample_rate, &cur_opts, sel_cand, MAX_CAND);
        sel_pending = deck->phrases_pending || deck->grid_pending ||
                      an_availability_pending(deck->hot_cues) || an_availability_pending(deck->memory_cues);
    }
    if (strip) C->surface_redraw(strip);
}

static void open_selector(void)
{
    if (cur_a < 0 || !decks[cur_a].position_valid) return;
    sel_open = 1;
    sel_b = sel_page = 0;
    sel_deck = cur_a;
    refresh_selector();
}

static void open_b_selector(int kind)
{
    if (cur_b < 0 || !decks[cur_b].loaded) return;
    sel_b = sel_open = 1;
    sel_kind = kind;
    sel_page = 0;
    sel_deck = cur_b;
    refresh_selector();
}

static void set_b_target(const an_cand_t *target)
{
    if (!sel_open || cur_b < 0 || sel_deck != cur_b) return;
    b_targets[cur_b].target = *target;
    b_targets[cur_b].valid = 1;
    decks[cur_b].loop.released = 0;
    close_selector();
    shown_display_valid = 0;
    update_guide(mono_ms());
}

static void set_b_here(void)
{
    if (cur_b < 0 || !decks[cur_b].position_valid) return;
    an_cand_t target = { .ms = decks[cur_b].pos_ms, .label = "MARKED POINT" };
    set_b_target(&target);
}

static void latch_candidate(int idx)
{
    if (!sel_open || idx < 0 || idx >= sel_n) return;
    if (sel_b) {
        set_b_target(&sel_cand[idx]);
        return;
    }
    if (cur_a < 0 || sel_deck != cur_a) return;
    latched_ms = sel_cand[idx].ms;
    snprintf(latched_label, sizeof latched_label, "%s", sel_cand[idx].label);
    decks[cur_b].loop.released = 0;
    close_selector();
    shown_display_valid = 0;
    update_guide(mono_ms());
}

static int selector_back(void)
{
    if (!sel_open) return 0;
    close_selector();
    if (strip) C->surface_redraw(strip);
    return 1;
}

static int selector_open_tap(const az_event *ev)
{
    if (cur_a < 0 || cur_b < 0 || !plan.valid) return 1;
    if (controls_ok && in_rect(&target_controls[1], ev->x, ev->y)) open_b_selector(0);
    else open_selector();
    return 1;
}

static int selector_kind_tap(int x, int y)
{
    if (!sel_b) return 0;
    for (int i = 0; i < 3; i++) {
        if (!in_rect(&kind_r[i], x, y)) continue;
        if (i == 2) set_b_here();
        else open_b_selector(i);
        return 1;
    }
    return 0;
}

static int selector_candidate_tap(int x, int y)
{
    for (int i = 0; i < 4 && i < sel_per; i++) {
        if (!in_rect(&chip_r[i], x, y)) continue;
        latch_candidate(sel_page * sel_per + i);
        return 1;
    }
    return 0;
}

static void selector_navigation_tap(int x, int y)
{
    if (in_rect(&arrow_l_r, x, y)) {
        if (sel_page > 0) sel_page--;
    } else if (in_rect(&arrow_r_r, x, y)) {
        int pages = (sel_n + sel_per - 1) / sel_per;
        if (sel_page + 1 < pages) sel_page++;
    } else close_selector();
    if (strip) C->surface_redraw(strip);
}

static int selector_tap(const az_event *ev)
{
    if (!sel_open) return selector_open_tap(ev);
    if (!layout_ok) {
        close_selector();
        return 1;
    }
    if (selector_kind_tap(ev->x, ev->y) || selector_candidate_tap(ev->x, ev->y)) return 1;
    selector_navigation_tap(ev->x, ev->y);
    return 1;
}

static int input(const az_event *ev, void *user)
{
    (void)user;
    if (!C || !ev || !settings_get(SET_SHOW)) return 0;
    if (ev->kind == AZ_EV_BACK) return selector_back();
    if (ev->kind == AZ_EV_TAP) return selector_tap(ev);
    return 0;
}

static int paused(const an_deck_status_t *deck)
{
    return deck->playback == AZ_PLAYBACK_PAUSED || deck->playback == AZ_PLAYBACK_STOPPED;
}

static void choose_playing_decks(int *a, int *b)
{
    /* Starting or leaving a loop must not reverse an established transition. */
    *a = cur_a;
    *b = cur_b;
    if (*a >= 0 && *b >= 0) return;
    for (int d = 0; d < AN_DECKS; d++) {
        const an_deck_status_t *other = &decks[1 - d];
        if (decks[d].loop_engaged && !other->loop_engaged && other->loop_availability == AZ_AVAIL_READY) {
            *b = d;
            *a = 1 - d;
            return;
        }
    }
}

static int playback_known(const an_deck_status_t *deck)
{
    return deck->playback == AZ_PLAYBACK_PLAYING || paused(deck);
}

static void choose_requested_decks(int incoming, int *a, int *b)
{
    int outgoing = 2 - incoming;
    if (decks[outgoing].playback == AZ_PLAYBACK_PLAYING) {
        *a = outgoing;
        *b = 1 - outgoing;
    }
}

static void choose_decks(int incoming, int *a, int *b)
{
    *a = *b = -1;
    if (!decks[0].loaded || !decks[1].loaded) return;
    int p0 = decks[0].playback == AZ_PLAYBACK_PLAYING;
    int p1 = decks[1].playback == AZ_PLAYBACK_PLAYING;
    if (!playback_known(&decks[0]) || !playback_known(&decks[1])) return;
    if (incoming) {
        choose_requested_decks(incoming, a, b);
    } else if (p0 != p1) {
        *a = p0 ? 0 : 1;
        *b = 1 - *a;
    } else if (p0) {
        choose_playing_decks(a, b);
    }
}

static int update_options(void)
{
    int phrase = settings_get(SET_PHRASE);
    an_target_opts_t opts = {
        .rule = settings_get(SET_RULE),
        .phrase_cat = phrase ? phrase : -1,
        .phrase_point = settings_get(SET_POINT),
        .consolidate = settings_get(SET_CONSOL),
        .hotcue_any = settings_get(SET_HCANY),
        .hotcue_slot = settings_get(SET_HCSLOT) + 1,
    };
    int incoming = settings_get(SET_DECK);
    if (incoming != cur_deck_setting) cur_a = cur_b = -1;
    if (incoming != cur_deck_setting || memcmp(&opts, &cur_opts, sizeof opts) != 0) {
        cur_opts = opts;
        cur_deck_setting = incoming;
        latched_ms = 0;
        close_selector();
        for (int d = 0; d < AN_DECKS; d++) decks[d].loop.released = 0;
    }

    return incoming;
}

static void update_sidebar_tab(int sidebar, int show)
{
    if (sidebar_tab && sidebar != tab_shown) {
        C->surface_show(sidebar_tab, sidebar);
        tab_shown = sidebar;
    }
    if (sidebar_tab && sidebar && show != tab_open) {
        tab_open = show;
        C->surface_place(sidebar_tab, an_sidebar_tab_rect(show));
        C->surface_redraw(sidebar_tab);
    }
}

static int update_visibility(int layout, int a)
{
    int show = settings_get(SET_SHOW);
    int sidebar = layout == ANCHOR_LAYOUT_SIDEBAR;
    update_sidebar_tab(sidebar, show);

    int visible = show && (sidebar || a >= 0);
    if (visible != strip_shown) {
        strip_shown = visible;
        C->surface_show(strip, visible);
        shown_display_valid = 0;
    }
    if (!visible) {
        close_selector();
        return 0;
    }

    return 1;
}

static void refresh_plan(uint64_t now, int a, int b)
{
    if (a >= 0) {
        compute_plan(now, a, b, &cur_opts);
        if (b_targets[b].valid) {
            plan.b_target_valid = 1;
            plan.b_target_ms = b_targets[b].target.ms;
            snprintf(plan.b_target, sizeof plan.b_target, "%s", b_targets[b].target.label);
        }
    } else if (!decks[0].loaded || !decks[1].loaded) {
        notice(AN_ST_NEED_TRACKS, AN_NOTICE_NONE, 0, "Load Decks 1 and 2");
    } else if (paused(&decks[0]) && paused(&decks[1])) {
        notice(AN_ST_NONE, AN_NOTICE_START_OUTGOING, 0, "Start the outgoing track");
    } else if (decks[0].playback != AZ_PLAYBACK_PLAYING || decks[1].playback != AZ_PLAYBACK_PLAYING) {
        notice(AN_ST_OFF, AN_NOTICE_DECK_STATE, 0, "Waiting for playable decks");
    } else {
        notice(AN_ST_NONE, AN_NOTICE_NONE, 2, "Choose the incoming deck");
    }
}

static void redraw_plan(int a, int b)
{
    an_display_t display;
    an_display_model(&plan, a, b, &display);
    if (!shown_display_valid || memcmp(&shown_display, &display, sizeof display) != 0 ||
        (sel_open && strcmp(shown_line2, plan.line2) != 0)) {
        C->surface_redraw(strip);
        snprintf(shown_line2, sizeof shown_line2, "%s", plan.line2);
        shown_display = display;
        shown_display_valid = 1;
    }
}

static void update_guide(uint64_t now)
{
    if (!strip) return;
    int layout = selected_layout();
    if (layout != cur_layout) {
        cur_layout = layout;
        close_selector();
        C->surface_place(strip, an_display_rect(layout));
        shown_display_valid = 0;
    }

    int incoming = update_options();

    int a, b;
    choose_decks(incoming, &a, &b);
    if (a != cur_a) latched_ms = 0;
    if (a != cur_a || b != cur_b) close_selector();
    cur_a = a;
    cur_b = b;

    if (!update_visibility(layout, a)) return;

    refresh_plan(now, a, b);

    redraw_plan(a, b);
}

static void settings_changed(void)
{
    update_guide(mono_ms());
}

static void deck_changed(const az_deck_status_event_v1 *event, void *user)
{
    (void)user;
    unsigned changed = an_deck_update(decks, event);
    if (!changed) return;
    int deck = (int)event->deck;
    if (changed & AN_DECK_REPLACED) invalidate_deck(deck);
    if ((changed & AN_DECK_ANALYSIS) && sel_open && sel_deck == deck) refresh_selector();
    update_guide(mono_ms());
}

static int surface_services_ready(const az_core_v1 *core)
{
    return AZ_HAS(core, surface_create) && AZ_HAS(core, surface_place) && AZ_HAS(core, surface_show) && AZ_HAS(core, surface_redraw);
}

static int drawing_services_ready(const az_core_v1 *core)
{
    return AZ_HAS(core, draw_text) && AZ_HAS(core, fill_rect) && AZ_HAS(core, draw_rect) && AZ_HAS(core, draw_image) &&
           AZ_HAS(core, text_width) && AZ_HAS(core, color);
}

static int core_services_ready(const az_core_v1 *core)
{
    return AZ_HAS(core, observation_service) && surface_services_ready(core) && drawing_services_ready(core) && AZ_HAS(core, request_redraw);
}

static void register_settings(const az_screen_v1 *settings)
{
    if (AZ_HAS(C, register_screen)) C->register_screen(MOD_ID, settings);
    if (AZ_HAS(C, register_row)) {
        const az_row_v1 setup_row = {
            .size = sizeof setup_row,
            .key = "settings",
            .label = ANCHOR_MOD_NAME " settings",
            .kind = AZ_ROW_SCREEN,
            .screen = settings,
            .help = "How to use the guide, and every transition setting",
        };
        C->register_row(MOD_ID, &setup_row);
    }
}

static int init(const az_core_v1 *core)
{
    if (!core_services_ready(core)) return -1;
    const az_observation_v1 *lib = core->observation_service(AZ_OBSERVATION_ABI);
    if (!AZ_OBSERVATION_HAS(lib, on_deck_status) || !AZ_OBSERVATION_HAS(lib, deck_status_release)) {
        if (AZ_HAS(core, log)) core->log(MOD_ID, "Deck status requires AZ-MODS " AZ_DECK_STATUS_MIN_CORE);
        return -1;
    }

    C = core;
    const az_screen_v1 *settings = settings_init(C, settings_changed);
    static const az_surface_v1 guide = {
        .size = sizeof guide,
        .layer = AZ_LAYER_OVER_EP147,
        .views = AZ_VIEW_TWO_DECKS,
        .on_paint = paint,
        .on_input = input,
    };
    static const az_surface_v1 tab = {
        .size = sizeof tab,
        .layer = AZ_LAYER_OVER_EP147,
        .views = AZ_VIEW_TWO_DECKS,
        .on_paint = tab_paint,
        .on_input = tab_input,
    };
    strip = C->surface_create(MOD_ID, &guide);
    sidebar_tab = C->surface_create(MOD_ID, &tab);
    if (!strip || !sidebar_tab) return -1;

    static const az_deck_watch_v1 watch = {
        .size = sizeof watch,
        .version = AZ_DATA_VERSION,
        .deck_mask = (1u << AN_DECKS) - 1,
        .position_hz = 30,
        .topics = AZ_STATUS_TOPIC(AZ_STATUS_LOAD) | AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) |
                  AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_TEMPO) |
                  AZ_STATUS_TOPIC(AZ_STATUS_LOOP) | AZ_STATUS_TOPIC(AZ_STATUS_GRID) |
                  AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) | AZ_STATUS_TOPIC(AZ_STATUS_CUES),
    };
    int rc = lib->on_deck_status(MOD_ID, &watch, deck_changed, NULL, &subscription);
    if (rc != AZ_OK) {
        if (AZ_HAS(C, log)) C->log(MOD_ID, "Deck status subscription refused (%d)", rc);
        return rc;
    }

    register_settings(settings);
    update_guide(mono_ms());
    return 0;
}

static const az_mod_v1 MOD = {
    .abi = AZ_MOD_ABI,
    .size = sizeof MOD,
    .id = MOD_ID,
    .fw_tags = TAGS,
    .init = init,
};

AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void)
{
    return &MOD;
}
