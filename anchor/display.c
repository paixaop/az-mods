/* SPDX-License-Identifier: GPL-3.0-only
 * Action-first presentation for Anchor's three selectable layouts. No deck control. */
#include "display.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

az_rect an_display_rect(int layout)
{
    if (layout == ANCHOR_LAYOUT_CARD) return (az_rect){ 188, 430, 884, 128 };
    /* Open over the native Beat FX panel, flush with the screen's right edge. */
    if (layout == ANCHOR_LAYOUT_SIDEBAR) return (az_rect){ 1090, 89, 190, 333 };
    return (az_rect){ 188, 230, 872, 54 };
}

az_rect an_sidebar_tab_rect(int expanded)
{
    return expanded ? (az_rect){ 1054, 292, 26, 48 } : (az_rect){ 1195, 292, 85, 48 };
}

static void countdown(an_display_t *m, double bars, const char *verb, int incoming)
{
    /* Ceil rather than round: never advertise an action before its beat. */
    int beats = (int)ceil(fmax(0.0, fmin(bars * 4.0, 39996.0)));
    if (beats < 1) beats = 1;
    int n = (beats + 3) / 4;
    int beat = (beats - 1) % 4 + 1;
    m->count_unit = bars <= 1.0 ? AN_COUNT_BEATS : AN_COUNT_BARS;
    snprintf(m->number, sizeof m->number, "%d", n);
    snprintf(m->beat, sizeof m->beat, ".%d", beat);
    snprintf(m->unit, sizeof m->unit, "%s", n == 1 ? "BAR" : "BARS");
    snprintf(m->heading, sizeof m->heading, "%s IN", verb);
    snprintf(m->headline, sizeof m->headline, "%s IN %s%s", verb, m->number, m->beat);
    if (!strcmp(verb, "PLAY"))
        snprintf(m->ribbon, sizeof m->ribbon, "PLAY DECK %d IN %s%s", incoming + 1, m->number, m->beat);
    else
        snprintf(m->ribbon, sizeof m->ribbon, "DECK %d - %s", incoming + 1, m->headline);
    if (m->looping) snprintf(m->ribbon, sizeof m->ribbon, "DECK %d LOOPING / %.78s", incoming + 1, m->headline);
}

static void message(an_display_t *m, const char *s)
{
    snprintf(m->heading, sizeof m->heading, "%s", s);
    snprintf(m->headline, sizeof m->headline, "%s", s);
    snprintf(m->ribbon, sizeof m->ribbon, "%s - %s", m->deck, s);
}

static int model_ready(const an_plan_t *p, int incoming, an_display_t *m)
{
    if (!p) {
        message(m, "LOAD TWO TRACKS");
        return 0;
    }
    if (p->state == AN_ST_NEED_TRACKS) {
        message(m, "LOAD TWO TRACKS");
        snprintf(m->detail, sizeof m->detail, "Load Decks 1 + 2; cue B before its drop");
        return 0;
    }
    if (p->notice == AN_NOTICE_START_OUTGOING) {
        message(m, "PLAY OUTGOING DECK");
        snprintf(m->detail, sizeof m->detail, "Play one track; cue the other before its drop");
        return 0;
    }
    if (p->notice == AN_NOTICE_DECK_STATE && incoming < 0) {
        message(m, "READING DECKS");
        return 0;
    }
    if (incoming < 0 || incoming >= 4) {
        message(m, "SELECT INCOMING DECK");
        snprintf(m->detail, sizeof m->detail, "Choose Incoming deck in Anchor settings");
        m->accent = 2;
        return 0;
    }
    return 1;
}

static void describe_target(const an_plan_t *p, int outgoing, int incoming, an_display_t *m)
{
    m->accent = p->accent;
    m->looping = p->looping;
    m->loop_prepared = p->loop_prepared;
    if (p->b_target_valid) {
        unsigned sec = p->b_target_ms / 1000;
        snprintf(m->b_target, sizeof m->b_target, "B: %.14s %u:%02u", p->b_target, sec / 60, sec % 60);
    } else snprintf(m->b_target, sizeof m->b_target, "B TARGET");
    if (m->looping) snprintf(m->deck, sizeof m->deck, "DECK %d LOOPING", incoming + 1);
    else if (m->loop_prepared) snprintf(m->deck, sizeof m->deck, "DECK %d LOOP READY", incoming + 1);
    if (outgoing >= 0 && outgoing < 4 && p->target[0])
        snprintf(m->detail, sizeof m->detail, "Target: Deck %d %s", outgoing + 1, p->target);
    else
        snprintf(m->detail, sizeof m->detail, "%s", p->line2);
}

static const char *const off_notices[] = {
    [AN_NOTICE_B_TARGET_PASSED] = "B TARGET PASSED",
    [AN_NOTICE_DECK_STATE] = "READING DECKS",
    [AN_NOTICE_LOOP_AFTER_DROP] = "LOOP BEFORE TARGET",
    [AN_NOTICE_HEAD_PAST_LOOP] = "CUE AT LOOP START",
    [AN_NOTICE_LOOP_UNKNOWN] = "READING LOOP",
    [AN_NOTICE_LOOP_UNAVAILABLE] = "LOOP STATE UNAVAILABLE",
};

static const char *const analysis_notices[] = {
    [AN_NOTICE_READING] = "READING TRACKS",
    [AN_NOTICE_NO_DROP] = "SET B TARGET",
    [AN_NOTICE_NO_CUES] = "ADD HOT CUES",
    [AN_NOTICE_CUES_UNAVAILABLE] = "HOT CUES UNAVAILABLE",
};

static const char *notice_message(const char *const *messages, size_t count, int notice, const char *fallback)
{
    if (notice < 0 || (size_t)notice >= count || !messages[notice]) return fallback;
    return messages[notice];
}

static void analysis_detail(const an_plan_t *p, int outgoing, int incoming, an_display_t *m)
{
        /* Error text identifies A/B in the existing detailed explanation. */
    if (p->notice == AN_NOTICE_ANALYSIS_A)
        snprintf(m->detail, sizeof m->detail, "Analyse Deck %d in rekordbox, then reload", outgoing + 1);
    else if (p->notice == AN_NOTICE_ANALYSIS_B)
        snprintf(m->detail, sizeof m->detail, "Analyse Deck %d in rekordbox, then reload", incoming + 1);
    else if (p->notice == AN_NOTICE_NO_CUES)
        snprintf(m->detail, sizeof m->detail, "Add cues to Deck %d or choose a phrase target", outgoing + 1);
}

static void analysis_message(const an_plan_t *p, int outgoing, int incoming, an_display_t *m)
{
    if ((p->notice == AN_NOTICE_ANALYSIS_A || p->notice == AN_NOTICE_NO_CUES ||
         p->notice == AN_NOTICE_CUES_UNAVAILABLE) &&
        outgoing >= 0 && outgoing < 4)
        snprintf(m->deck, sizeof m->deck, "DECK %d", outgoing + 1);
    message(m, notice_message(analysis_notices, sizeof analysis_notices / sizeof analysis_notices[0], p->notice, "ANALYSE TRACK"));
    analysis_detail(p, outgoing, incoming, m);
}

static void play_message(const an_plan_t *p, int incoming, an_display_t *m)
{
    if (p->play_now) {
        message(m, "PLAY NOW");
        snprintf(m->ribbon, sizeof m->ribbon, "PLAY DECK %d NOW", incoming + 1);
    } else countdown(m, p->bars_action, "PLAY", incoming);
}

static void target_message(const an_plan_t *p, int incoming, an_display_t *m)
{
    if (p->state == AN_ST_RELEASED) {
        if (p->bars_action <= 0) message(m, "B TARGET NOW");
        else countdown(m, p->bars_action, "TARGET", incoming);
    } else if (p->drop_aligned) countdown(m, p->bars_action, "TARGET", incoming);
    else message(m, "LOOP TO ALIGN");
}

static void immediate_message(int state, an_display_t *m)
{
    static const char *const labels[] = {
        [AN_ST_OPEN] = "EXIT LOOP NOW",
        [AN_ST_MISSED] = "PICK A LATER TARGET",
        [AN_ST_NEED_TRACKS] = "LOAD TWO TRACKS",
    };
    message(m, notice_message(labels, sizeof labels / sizeof labels[0], state, "CHOOSE A TARGET"));
}

static void state_message(const an_plan_t *p, int outgoing, int incoming, an_display_t *m)
{
    switch (p->state) {
    case AN_ST_SINGLE:
    case AN_ST_START: play_message(p, incoming, m); break;
    case AN_ST_WAIT: countdown(m, p->bars_action, "EXIT", incoming); break;
    case AN_ST_RELEASED:
    case AN_ST_PLAYING: target_message(p, incoming, m); break;
    case AN_ST_OFF:
        message(m, notice_message(off_notices, sizeof off_notices / sizeof off_notices[0], p->notice, "RESTART THIS DECK"));
        break;
    case AN_ST_NO_ANALYSIS: analysis_message(p, outgoing, incoming, m); break;
    default: immediate_message(p->state, m); break;
    }
}

void an_display_model(const an_plan_t *p, int outgoing, int incoming, an_display_t *m)
{
    memset(m, 0, sizeof *m);
    snprintf(m->deck, sizeof m->deck, incoming >= 0 && incoming < 4 ? "DECK %d" : "INCOMING", incoming + 1);
    if (!model_ready(p, incoming, m)) return;
    describe_target(p, outgoing, incoming, m);
    state_message(p, outgoing, incoming, m);
    if (p->tempo_mismatch) {
        m->number[0] = m->beat[0] = m->unit[0] = 0;
        m->count_unit = AN_COUNT_NONE;
        message(m, "MATCH DECK TEMPOS");
        snprintf(m->detail, sizeof m->detail, "Match tempos before following the countdown");
        m->accent = 2;
    }
}

static int width(const az_core_v1 *c, const char *s, int px)
{
    int w = c->text_width(s, px);
    return w > 0 ? w : (int)strlen(s) * px * 3 / 5;
}

static int fitted_font(const az_core_v1 *c, az_rect r, const char *s, int desired, int minimum)
{
    int px = desired > 96 ? 96 : desired;
    while (px > 8 && px * 3 / 2 > r.h) px--;
    if (minimum > px) minimum = px;
    while (px > minimum && width(c, s, px) > r.w) px--;
    return px;
}

static int ellipsized_text(const az_core_v1 *c, az_rect r, const char *s, int px, char buf[128])
{
    snprintf(buf, 128, "%s", s);
    if (width(c, buf, px) > r.w) {
        size_t len = strlen(buf);
        while (len && width(c, buf, px) > r.w) {
            len--;
            snprintf(buf, 128, "%.*s...", (int)len, s);
        }
        if (width(c, buf, px) > r.w) return 0;
    }
    return 1;
}

/* Fit action text by font size first; truncate only for unusually small test areas.
 * All strings built here are bounded, and paint only through SDK draw helpers. */
static void line(const az_core_v1 *c, az_draw *d, az_rect r, const char *s,
                 int desired, int minimum, uint32_t col, int center)
{
    if (r.w < 8 || r.h < 12 || !s[0]) return;
    int px = fitted_font(c, r, s, desired, minimum);
    if (px < 8) return;
    char buf[128];
    if (!ellipsized_text(c, r, s, px, buf)) return;
    int x = r.x + (center ? (r.w - width(c, buf, px)) / 2 : 0);
    c->draw_text(d, x, r.y + (r.h - px * 3 / 2) / 2, buf, px, col);
}

static uint32_t ink(int accent)
{
    if (accent == 4) return 0x34e27a;
    if (accent == 3) return 0xff6258;
    if (accent == 2) return 0xffbd54;
    if (accent == 0) return 0xb0b8c0;
    return 0x00d6f5;
}

static double tab_coverage(az_rect a, int x, int y)
{
    double coverage = 1;
    int edge_y = y < a.h / 2 ? y : a.h - 1 - y;
    if (x < 4 && edge_y < 4) {
        double dx = 3.5 - x, dy = 3.5 - edge_y;
        coverage = fmax(0, fmin(1, 4.5 - sqrt(dx * dx + dy * dy)));
    }
    return coverage;
}

static void tab_background(const az_core_v1 *c, az_draw *d, az_rect a)
{
    /* Native FX tab: translucent charcoal; do not paint the surrounding handle gap. */
    unsigned char rgba[85 * 48 * 4];
    int blended = 0;
    if (a.w <= 85 && a.h <= 48) {
        for (int i = 0; i < a.w * a.h; i++) {
            int x = i % a.w, y = i / a.w;
            double coverage = tab_coverage(a, x, y);
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = (unsigned char)llround(40 * coverage);
            rgba[i * 4 + 3] = (unsigned char)llround(204 * coverage);
        }
        blended = c->draw_image(d, a.x, a.y, rgba, a.w, a.h) == 0;
    }
    if (!blended) c->fill_rect(d, a.x, a.y, a.w, a.h, 0x282828);
}

void an_sidebar_tab_draw(const az_core_v1 *c, az_draw *d, az_rect a, int expanded)
{
    if (!c || !d || a.w < 24 || a.h < 32) return;
    tab_background(c, d, a);
    int x = a.x + (expanded ? (a.w - 9) / 2 : 8), cy = a.y + a.h / 2;
    for (int i = 0; i < 9; i++) {
        int half = expanded ? 8 - i : i;
        c->fill_rect(d, x + i, cy - half, 1, 2 * half + 1, 0xffffff);
    }
    if (!expanded) line(c, d, (az_rect){ a.x + 22, a.y, a.w - 25, a.h }, "ANCHOR", 14, 11, 0xffffff, 1);
}

static void wide_controls(az_rect a, int layout, az_rect buttons[2])
{
    if (layout == ANCHOR_LAYOUT_CARD && a.h >= 120) {
        int w = (a.w - 30) / 2;
        for (int i = 0; i < 2; i++)
            buttons[i] = (az_rect){ a.x + 12 + i * (w + 6), a.y + a.h - 40, w, 36 };
    } else {
        int widths[] = { 128, 200 };
        int x = a.x + a.w - 12 - 334;
        for (int i = 0; i < 2; i++) {
            buttons[i] = (az_rect){ x, a.y + (a.h - 44) / 2, widths[i], 44 };
            x += widths[i] + 6;
        }
    }
}

void an_display_controls(az_rect a, int layout, az_rect buttons[2])
{
    memset(buttons, 0, sizeof(az_rect) * 2);
    if (layout == ANCHOR_LAYOUT_SIDEBAR && a.w >= 140 && a.h >= 320) {
        for (int i = 0; i < 2; i++)
            buttons[i] = (az_rect){ a.x + 12, a.y + a.h - 82 + i * 41, a.w - 24, 36 };
    } else if (a.w >= 620 && a.h >= 44) {
        wide_controls(a, layout, buttons);
    }
}

/* Keep the existing bar font and baseline; only the dot/beat use a smaller font. */
static void counter(const az_core_v1 *c, az_draw *d, az_rect r, const an_display_t *m,
                    const char *prefix, int desired, int minimum, uint32_t col, int center)
{
    int px = desired;
    if (px > 96) px = 96;
    int small, total;
    do {
        small = px * 55 / 100;
        if (small < 8) small = 8;
        total = width(c, prefix, px) + width(c, m->number, px) + width(c, m->beat, small);
        if (total <= r.w && px * 3 / 2 <= r.h) break;
        px--;
    } while (px >= minimum);
    if (px < minimum) {
        char text[128];
        snprintf(text, sizeof text, "%s%s%s", prefix, m->number, m->beat);
        line(c, d, r, text, minimum, 8, col, center);
        return;
    }
    int x = r.x + (center ? (r.w - total) / 2 : 0);
    int y = r.y + (r.h - px * 3 / 2) / 2;
    if (prefix[0]) c->draw_text(d, x, y, prefix, px, col);
    x += width(c, prefix, px);
    c->draw_text(d, x, y, m->number, px, col);
    x += width(c, m->number, px);
    c->draw_text(d, x, y + px - small, m->beat, small, col);
}

static void controls(const az_core_v1 *c, az_draw *d, az_rect a, int layout,
                     const an_display_t *m)
{
    az_rect buttons[2];
    an_display_controls(a, layout, buttons);
    const char *a_label = layout == ANCHOR_LAYOUT_RIBBON ? "A TARGET" : m->detail;
    if (!a_label[0] || strncmp(a_label, "Target:", 7)) a_label = "A TARGET";
    if (layout == ANCHOR_LAYOUT_RIBBON) a_label = "A TARGET";
    else if (!strncmp(a_label, "Target: ", 8)) a_label += 8;
    const char *labels[] = { a_label, m->b_target[0] ? m->b_target : "B TARGET" };
    for (int i = 0; i < 2; i++) {
        az_rect b = buttons[i];
        if (!b.w) continue;
        c->draw_rect(d, b.x, b.y, b.w, b.h, 1, 0x8797a3);
        b.x += 4;
        b.w -= 8;
        line(c, d, b, labels[i], 14, 11, 0xdce4ea, 1);
    }
}

typedef struct {
    uint32_t accent, count, action;
} display_colors_t;

static display_colors_t display_colors(const an_display_t *m)
{
    uint32_t accent = (m->looping || m->loop_prepared) && m->accent < 2 ? AN_COLOR_LOOPING : ink(m->accent);
    uint32_t count = m->count_unit == AN_COUNT_BEATS ? AN_COLOR_BEATS : AN_COLOR_BARS;
    uint32_t action = m->count_unit ? count : m->accent >= 2 ? accent
                                                             : 0xffffff;
    return (display_colors_t){ accent, count, action };
}

static void sidebar_lines(const char *headline, const char **first, const char **second)
{
    static const struct {
        const char *headline, *first, *second;
    } labels[] = {
        { "PLAY NOW", "PLAY", "NOW" },
        { "EXIT LOOP NOW", "EXIT LOOP", "NOW" },
        { "B TARGET PASSED", "B TARGET", "PASSED" },
        { "B TARGET NOW", "B TARGET", "NOW" },
        { "MATCH DECK TEMPOS", "MATCH", "TEMPOS" },
        { "PICK A LATER TARGET", "PICK A LATER", "TARGET" },
        { "RESTART THIS DECK", "RESTART", "THIS DECK" },
        { "ANALYSE TRACK", "ANALYSE", "TRACK" },
        { "READING TRACKS", "READING", "TRACKS" },
        { "SET B TARGET", "SET B", "TARGET" },
        { "CUE AT LOOP START", "CUE AT", "LOOP START" },
        { "CHOOSE A TARGET", "CHOOSE A", "TARGET" },
        { "LOOP TO ALIGN", "LOOP", "TO ALIGN" },
        { "LOOP BEFORE TARGET", "LOOP BEFORE", "TARGET" },
        { "LOAD TWO TRACKS", "LOAD TWO", "TRACKS" },
        { "PLAY OUTGOING DECK", "PLAY THE", "OUTGOING" },
        { "READING DECKS", "READING", "DECKS" },
        { "READING LOOP", "READING", "LOOP" },
        { "LOOP STATE UNAVAILABLE", "LOOP STATE", "UNAVAILABLE" },
        { "HOT CUES UNAVAILABLE", "HOT CUES", "UNAVAILABLE" },
        { "SELECT INCOMING DECK", "SELECT", "INCOMING DECK" },
    };
    *first = headline;
    *second = "";
    for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++) {
        if (strcmp(headline, labels[i].headline)) continue;
        *first = labels[i].first;
        *second = labels[i].second;
        return;
    }
}

static void draw_ribbon(const az_core_v1 *c, az_draw *d, az_rect a, int layout, const an_display_t *m, display_colors_t colors)
{
    int pad = 12;
    az_rect buttons[2];
    an_display_controls(a, layout, buttons);
    int right = buttons[0].w ? buttons[0].x - 8 : a.x + a.w - pad;
    az_rect r = { a.x + pad, a.y + 2, right - a.x - pad, a.h - 4 };
    if (m->number[0]) {
        char prefix[112];
        snprintf(prefix, sizeof prefix, "%s", m->ribbon);
        char *last = strrchr(prefix, ' ');
        if (last) last[1] = 0;
        counter(c, d, r, m, prefix, 32, 16, colors.action, 0);
    } else line(c, d, r, m->ribbon, 32, 16, colors.action, 0);
    controls(c, d, a, layout, m);
}

static void draw_card(const az_core_v1 *c, az_draw *d, az_rect a, int layout, const an_display_t *m, display_colors_t colors)
{
    int pad = 12;
    line(c, d, (az_rect){ a.x + pad, a.y + 4, a.w - 2 * pad, 20 }, m->deck, 13, 10,
         m->looping || m->loop_prepared ? AN_COLOR_LOOPING : colors.accent, 0);
    az_rect r = { a.x + pad, a.y + 24, a.w - 2 * pad, a.h - 68 };
    if (m->number[0]) {
        char prefix[64];
        snprintf(prefix, sizeof prefix, "%s ", m->heading);
        counter(c, d, r, m, prefix, 40, 24, colors.action, 0);
    } else line(c, d, r, m->headline, 40, 24, colors.action, 0);
    controls(c, d, a, layout, m);
}

static void draw_sidebar(const az_core_v1 *c, az_draw *d, az_rect a, int layout, const an_display_t *m, display_colors_t colors)
{
    int pad = 12;
    int w = a.w - 2 * pad;
    line(c, d, (az_rect){ a.x + pad, a.y + 10, w, 24 }, m->deck, 15, 11,
         m->looping || m->loop_prepared ? AN_COLOR_LOOPING : colors.accent, 1);
    if (m->number[0]) {
        line(c, d, (az_rect){ a.x + pad, a.y + 44, w, 35 }, m->heading, 23, 15,
             m->looping ? AN_COLOR_LOOPING : 0xffffff, 1);
        counter(c, d, (az_rect){ a.x + pad, a.y + 80, w, 144 }, m, "", 96, 40, colors.count, 1);
        line(c, d, (az_rect){ a.x + pad, a.y + 224, w, 24 }, m->unit, 15, 11, colors.count, 1);
    } else {
            /* Immediate instructions stay large rather than shrinking into one long line. */
        const char *first, *second;
        sidebar_lines(m->headline, &first, &second);
        line(c, d, (az_rect){ a.x + pad, a.y + 67, w, 52 }, first, 30, 17, 0xffffff, 1);
        line(c, d, (az_rect){ a.x + pad, a.y + 124, w, 98 }, second, 54, 18, colors.accent, 1);
    }
        /* A target can be long: two measured lines, never over the colors.action or button. */
    if (!strcmp(m->headline, "LOAD TWO TRACKS") || !strcmp(m->headline, "PLAY OUTGOING DECK")) {
        line(c, d, (az_rect){ a.x + pad, a.y + a.h - 88, w, 18 }, "1 LOAD DECKS 1 + 2", 11, 9, 0xaeb9c3, 1);
        line(c, d, (az_rect){ a.x + pad, a.y + a.h - 68, w, 18 }, "2 PLAY OUTGOING", 11, 9, 0xaeb9c3, 1);
        line(c, d, (az_rect){ a.x + pad, a.y + a.h - 48, w, 18 }, "3 CUE B BEFORE DROP", 11, 9, 0xaeb9c3, 1);
        return;
    }
    controls(c, d, a, layout, m);
}

static int drawable(const az_core_v1 *c, const az_draw *d, const an_display_t *m, az_rect a)
{
    return c && d && m && a.w >= 80 && a.h >= 24;
}

void an_display_draw(const az_core_v1 *c, az_draw *d, az_rect a, int layout,
                     const an_display_t *m)
{
    if (!drawable(c, d, m, a)) return;
    display_colors_t colors = display_colors(m);
    c->fill_rect(d, a.x, a.y, a.w, a.h, 0x080b0e);
    c->draw_rect(d, a.x, a.y, a.w, a.h, 1, 0x374047);
    c->fill_rect(d, a.x, a.y, 4, a.h, colors.accent);
    if (layout == ANCHOR_LAYOUT_RIBBON || a.h < 100 ||
        (layout == ANCHOR_LAYOUT_SIDEBAR && a.h < 320)) draw_ribbon(c, d, a, layout, m, colors);
    else if (layout == ANCHOR_LAYOUT_CARD) draw_card(c, d, a, layout, m, colors);
    else draw_sidebar(c, d, a, layout, m, colors);
}
