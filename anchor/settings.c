/* SPDX-License-Identifier: GPL-3.0-only
 *
 * settings.c: Anchor's settings values and the page that edits them.
 *
 * Quick controls: visual guide placement and grouped transition settings. Fits the core's
 * available area, with paging on docked pages and 44px controls. A tap selects or changes
 * a value; the knob moves focus and its click changes the focused setting.
 */
#include "settings.h"
#include "mod.h"

#include <stdio.h>
#include <string.h>

static const char *const SET_RULE_LABELS[4] = { "Phrase", "Hot cue", "16-bar grid", "32-bar grid" };
static const char *const SET_DISPLAY_LABELS[3] = { "Ribbon", "Card", "Sidebar" };
static const char *const SET_PHRASE_LABELS[8] = { "Any", "Intro", "Build", "Break",
                                                "Chorus", "Outro", "Verse", "Bridge" };
static const char *const SET_POINT_LABELS[2] = { "Ending", "Start" };
static const char *const SET_HOTCUE_LABELS[8] = { "HC1", "HC2", "HC3", "HC4",
                                                "HC5", "HC6", "HC7", "HC8" };
static const char *const SET_DECK_LABELS[3] = { "Auto", "Deck 1", "Deck 2" };

static const az_core_v1 *C;
static void (*changed)(void);
static const az_screen_v1 settings_screen;

/* key, default, choices, labels */
typedef struct {
    int key;
    int def;
    int n;
    const char *const *labels;
} set_desc;

static const set_desc DESC[SET_COUNT] = {
    { SET_SHOW, 1, 0, NULL },
    { SET_DISPLAY, 1, 3, SET_DISPLAY_LABELS },
    { SET_RULE, 0, 4, SET_RULE_LABELS },
    { SET_PHRASE, 0, 8, SET_PHRASE_LABELS },
    { SET_POINT, 0, 2, SET_POINT_LABELS },
    { SET_CONSOL, 1, 0, NULL },
    { SET_HCANY, 1, 0, NULL },
    { SET_HCSLOT, 0, 8, SET_HOTCUE_LABELS },
    { SET_DECK, 0, 3, SET_DECK_LABELS },
};

static const char *const KEYS[SET_COUNT] = {
    "show", "display", "a-target", "a-phrase", "a-point", "a-consolidate",
    "a-hotcue-any", "a-hotcue-slot", "b-deck",
};

static const char *const ROW_LABELS[SET_COUNT] = {
    "Show guide",
    "Display",
    "A target",
    "Default phrase",
    "Phrase point",
    "Consolidate phrases",
    "Any hot cue",
    "Default hot cue",
    "Incoming deck",
};

static int values[SET_COUNT];
static int loaded_cfg;

const az_screen_v1 *settings_init(const az_core_v1 *core, void (*on_change)(void))
{
    C = core;
    changed = on_change;
    for (int i = 0; i < SET_COUNT; i++)
        values[i] = C && AZ_HAS(C, cfg_get) ? C->cfg_get(ANCHOR_MOD_ID, KEYS[i], DESC[i].def)
                                            : DESC[i].def;
    for (int i = 0; i < SET_COUNT; i++)
        if (values[i] < 0 || values[i] >= (DESC[i].n ? DESC[i].n : 2)) values[i] = DESC[i].def;
    loaded_cfg = 1;
    return &settings_screen;
}
int settings_get(int key)
{
    if (!loaded_cfg) return 0;
    if (key < 0 || key >= SET_COUNT) return 0;
    return values[key];
}
void settings_set(int key, int value)
{
    if (key < 0 || key >= SET_COUNT) return;
    int v = value;
    if (DESC[key].n > 0) {
        if (v < 0) v = DESC[key].n - 1;
        if (v >= DESC[key].n) v = 0;
    } else {
        v = v ? 1 : 0;
    }
    if (values[key] == v) return;
    values[key] = v;
    if (C && AZ_HAS(C, cfg_set)) C->cfg_set(ANCHOR_MOD_ID, KEYS[key], v);
    if (changed) changed();
}

/* ------------------------------------------------------------------------------------------
 * drawing helpers
 */
static int tw(const char *s, int px)
{
    if (!C || !s || !s[0]) return 0;
    int w = C->text_width(s, px);
    return w > 0 ? w : (int)strlen(s) * px * 3 / 5;
}

/* Copy s into out, cut at the last character that fits with "..." when it is too wide. */
static void fit(char *out, size_t n, const char *s, int px, int maxw)
{
    if (!n) return;
    out[0] = 0;
    if (!s) return;
    if (maxw <= 0) return;
    if (tw(s, px) <= maxw) {
        snprintf(out, n, "%s", s);
        return;
    }
    size_t len = strlen(s);
    if (len > n - 1) len = n - 1;
    const char *dots = "...";
    while (len > 0) {
        char tmp[160];
        snprintf(tmp, sizeof tmp, "%.*s%s", (int)len, s, dots);
        if (tw(tmp, px) <= maxw) {
            snprintf(out, n, "%s", tmp);
            return;
        }
        len--;
    }
    if (tw(dots, px) <= maxw) snprintf(out, n, "%s", dots);
}

static void draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t col)
{
    if (C && s && s[0]) C->draw_text(d, x, y, s, px, col);
}

/* One line of text, vertically centred in h, cut to maxw. */
static void line_fit(az_draw *d, int x, int y, int w, int h, const char *s, int px, uint32_t col)
{
    char buf[384];
    if (w <= 0 || h < px * 3 / 2) return;
    fit(buf, sizeof buf, s, px, w);
    draw_text(d, x, y + (h - px * 3 / 2) / 2, buf, px, col);
}

/* Quick controls uses the rectangle supplied by the core, not the panel dimensions.
 * A shorter/docked page keeps 44px targets and pages the settings instead of shrinking. */
enum { HIT_SET, HIT_CYCLE, HIT_TAB, HIT_PAGE };
typedef struct { az_rect r; int kind, key, value; } hit_t;
static hit_t hits[64];
static int nhits, g_sel = SET_SHOW, g_tab, g_first, g_visible, g_wide;
static int focus_keys[SET_COUNT], nfocus;
static az_rect g_area;

static uint32_t mix(uint32_t a, uint32_t b, unsigned pct)
{
    uint32_t col = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        unsigned x = (a >> shift) & 255, y = (b >> shift) & 255;
        col |= ((x * (100 - pct) + y * pct) / 100) << shift;
    }
    return col;
}
static uint32_t ink(void) { return C->color(AZ_COL_TEXT); }
static uint32_t dim(void) { return C->color(AZ_COL_TEXT_DIM); }
static uint32_t bg(void) { return C->color(AZ_COL_BG); }
static uint32_t accent(void) { return C->color(AZ_COL_HI); }

static int tab_for(int key)
{
    return key <= SET_DISPLAY ? 0 : key <= SET_HCSLOT ? 1 : 2;
}
static void rebuild_focus(void)
{
    nfocus = 0;
    focus_keys[nfocus++] = SET_SHOW;
    focus_keys[nfocus++] = SET_DISPLAY;
    focus_keys[nfocus++] = SET_RULE;
    if (settings_get(SET_RULE) == 0) {
        focus_keys[nfocus++] = SET_PHRASE;
        focus_keys[nfocus++] = SET_POINT;
        focus_keys[nfocus++] = SET_CONSOL;
    } else if (settings_get(SET_RULE) == 1) {
        focus_keys[nfocus++] = SET_HCANY;
        if (!settings_get(SET_HCANY)) focus_keys[nfocus++] = SET_HCSLOT;
    }
    focus_keys[nfocus++] = SET_DECK;
    int found = 0;
    for (int i = 0; i < nfocus; i++) found |= focus_keys[i] == g_sel;
    if (!found) g_sel = SET_RULE;
}
static void add_hit(int kind, int key, int value, az_rect r)
{
    if (nhits >= (int)(sizeof hits / sizeof hits[0]) || r.w <= 0 || r.h <= 0) return;
    hits[nhits++] = (hit_t){r, kind, key, value};
}
static void text(az_draw *d, az_rect r, const char *s, int px, uint32_t col)
{
    line_fit(d, r.x, r.y, r.w, r.h, s, px, col);
}
static void button(az_draw *d, az_rect r, const char *label, int key,
                   int kind, int value, int selected, int px)
{
    C->fill_rect(d, r.x, r.y, r.w, r.h, selected ? accent() : mix(bg(), ink(), 5));
    C->draw_rect(d, r.x, r.y, r.w, r.h, 1,
                 selected || key == g_sel ? accent() : mix(bg(), ink(), 26));
    char buf[160];
    fit(buf, sizeof buf, label, px, r.w - 12);
    int w = tw(buf, px);
    text(d, (az_rect){r.x + (r.w - w) / 2, r.y, w, r.h}, buf, px,
         selected ? C->color(AZ_COL_HI_TEXT) : ink());
    add_hit(kind, key, value, r);
}
static const char *value_label(int key)
{
    if (key < 0 || key >= SET_COUNT) return "";
    if (!DESC[key].n) return settings_get(key) ? "ON" : "OFF";
    int v = settings_get(key);
    return DESC[key].labels[v >= 0 && v < DESC[key].n ? v : 0];
}
static const char *label_for(int key)
{
    switch (key) {
    case SET_RULE: return "Target type";
    case SET_CONSOL: return "Join matching phrases";
    case SET_HCANY: return "Use any hot cue";
    default: return ROW_LABELS[key];
    }
}
static void value_control(az_draw *d, az_rect r, int key, int px)
{
    if (!DESC[key].n) {
        button(d, r, value_label(key), key, HIT_CYCLE, 1, settings_get(key), px);
        return;
    }
    /* 44px arrows work with gloves or an imprecise tap. The value also cycles forward. */
    if (r.w >= 170) {
        button(d, (az_rect){r.x, r.y, 44, r.h}, "<", key, HIT_CYCLE, -1, 0, px);
        button(d, (az_rect){r.x + 48, r.y, r.w - 96, r.h}, value_label(key),
               key, HIT_CYCLE, 1, 0, px);
        button(d, (az_rect){r.x + r.w - 44, r.y, 44, r.h}, ">", key, HIT_CYCLE, 1, 0, px);
    } else button(d, r, value_label(key), key, HIT_CYCLE, 1, 0, px);
}
static void setting_row(az_draw *d, az_rect r, int key, int px)
{
    int vw = r.w * 55 / 100;
    if (!DESC[key].n) vw = 76;
    text(d, (az_rect){r.x, r.y, r.w - vw - 12, r.h}, label_for(key), px, ink());
    value_control(d, (az_rect){r.x + r.w - vw, r.y, vw, r.h}, key, px);
}
static void stacked_field(az_draw *d, az_rect r, int key)
{
    text(d, (az_rect){r.x, r.y, r.w, 22}, label_for(key), 12, dim());
    value_control(d, (az_rect){r.x, r.y + 24, r.w, r.h - 24}, key, 16);
}
static void mini_placement(az_draw *d, az_rect r, int layout, uint32_t col)
{
    C->draw_rect(d, r.x, r.y, r.w, r.h, 1, mix(bg(), ink(), 26));
    int side = layout == 2 ? r.w / 5 : 0;
    C->fill_rect(d, r.x + 5, r.y + 6, r.w - 10 - side, 2, dim());
    C->fill_rect(d, r.x + 5, r.y + r.h / 2, r.w - 10 - side, 2, dim());
    az_rect guide = layout == 0 ? (az_rect){r.x + 5, r.y + r.h / 3, r.w - 10, 3} :
                    layout == 1 ? (az_rect){r.x + 5, r.y + r.h - 8, r.w - 10, 4} :
                    (az_rect){r.x + r.w - side - 3, r.y + 4, side, r.h - 8};
    C->fill_rect(d, guide.x, guide.y, guide.w, guide.h, col);
}
static void placements(az_draw *d, az_rect r)
{
    int gap = 8, w = (r.w - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        az_rect b = {r.x + i * (w + gap), r.y, w, r.h};
        int on = settings_get(SET_DISPLAY) == i;
        uint32_t col = on ? C->color(AZ_COL_HI_TEXT) : ink();
        C->fill_rect(d, b.x, b.y, b.w, b.h, on ? accent() : mix(bg(), ink(), 5));
        C->draw_rect(d, b.x, b.y, b.w, b.h, 1, on || g_sel == SET_DISPLAY ? accent() : dim());
        mini_placement(d, (az_rect){b.x + 12, b.y + 8, b.w - 24, b.h - 36}, i, col);
        text(d, (az_rect){b.x + 8, b.y + b.h - 25, b.w - 16, 24}, SET_DISPLAY_LABELS[i], 16, col);
        add_hit(HIT_SET, SET_DISPLAY, i, b);
    }
}
static void sample_preview(az_draw *d, az_rect r)
{
    C->fill_rect(d, r.x, r.y, r.w, r.h, mix(bg(), ink(), 4));
    C->draw_rect(d, r.x, r.y, r.w, r.h, 1, mix(bg(), ink(), 22));
    text(d, (az_rect){r.x + 10, r.y + 3, r.w - 20, 18}, "GUIDE PREVIEW / SAMPLE", 11, dim());
    int layout = settings_get(SET_DISPLAY), side = layout == 2 ? 110 : 0;
    int wavew = r.w - 24 - side;
    for (int row = 0; row < 2; row++) for (int i = 0; i < wavew / 7; i++) {
        int h = 3 + ((i * 13 + row * 7) % 11);
        C->fill_rect(d, r.x + 12 + i * 7, r.y + 25 + row * (layout == 0 ? 52 : 20) + (14 - h) / 2, 4, h,
                     mix(bg(), ink(), row ? 35 : 55));
    }
    if (!settings_get(SET_SHOW)) {
        text(d, (az_rect){r.x + 10, r.y + r.h - 28, r.w - 20, 24}, "Guide hidden", 14, dim());
        return;
    }
    az_rect guide = layout == 0 ? (az_rect){r.x + 9, r.y + 47, r.w - 18, 25} :
                    layout == 1 ? (az_rect){r.x + 9, r.y + r.h - 48, r.w - 18, 42} :
                    (az_rect){r.x + r.w - 110, r.y + 25, 100, r.h - 31};
    C->fill_rect(d, guide.x, guide.y, guide.w, guide.h, mix(bg(), accent(), 18));
    char instruction[80];
    int deck = settings_get(SET_DECK) == 1 ? 1 : 2;
    snprintf(instruction, sizeof instruction, "PLAY DECK %d IN 4 BARS", deck);
    if (layout == 2) {
        snprintf(instruction, sizeof instruction, "DECK %d / PLAY", deck);
        text(d, (az_rect){guide.x + 5, guide.y, guide.w - 10, 18}, instruction, 10, ink());
        text(d, (az_rect){guide.x + 5, guide.y + 19, guide.w - 10, 39}, "4 BARS", 20, accent());
    } else {
        text(d, (az_rect){guide.x + 8, guide.y, guide.w - 16, 24}, instruction, 16, ink());
        if (layout == 1) text(d, (az_rect){guide.x + 8, guide.y + 24, guide.w - 16, 17},
                              "TARGET / PHRASE END", 11, dim());
    }
}
static void page_controls(az_draw *d, az_rect r)
{
    int bw = 60;
    button(d, (az_rect){r.x, r.y, bw, r.h}, "<", -1, HIT_PAGE, -1, 0, 16);
    char label[80];
    snprintf(label, sizeof label, "%s / turn knob for more", g_tab == 0 ? "Guide" : g_tab == 1 ? "Targets" : "Incoming");
    text(d, (az_rect){r.x + bw + 8, r.y, r.w - 2 * bw - 16, r.h}, label, 11, dim());
    button(d, (az_rect){r.x + r.w - bw, r.y, bw, r.h}, ">", -1, HIT_PAGE, 1, 0, 16);
}
static void draw_wide(az_draw *d, az_rect a)
{
    int pad = 12, gap = 16;
    az_rect body = {a.x + pad, a.y + pad, a.w - 2 * pad, a.h - 2 * pad};
    int previeww = body.w * 42 / 100;
    int heroh = a.h >= 520 ? 140 : 112;
    sample_preview(d, (az_rect){body.x, body.y, previeww, heroh});
    int x = body.x + previeww + gap, w = body.w - previeww - gap;
    setting_row(d, (az_rect){x, body.y, w, 44}, SET_SHOW, 17);
    placements(d, (az_rect){x, body.y + 48, w, heroh - 48});
    int y = body.y + heroh + 12;
    int colw = (body.w - gap) / 2;
    int panelh = body.h - heroh - 12 - 52;
    int rowh = (panelh - 50) / 3;
    if (rowh > 64) rowh = 64;
    text(d, (az_rect){body.x, y, colw, 26}, "OUTGOING TARGET", 15, ink());
    text(d, (az_rect){body.x + colw + gap, y, colw, 26}, "INCOMING DECK", 15, ink());
    y += 28;
    setting_row(d, (az_rect){body.x, y, colw, rowh}, SET_RULE, 16);
    if (settings_get(SET_RULE) == 0) {
        int fw = (colw - 12) / 2;
        stacked_field(d, (az_rect){body.x, y + rowh + 4, fw, rowh + 24}, SET_PHRASE);
        stacked_field(d, (az_rect){body.x + fw + 12, y + rowh + 4, fw, rowh + 24}, SET_POINT);
        setting_row(d, (az_rect){body.x, y + 2 * rowh + 32, colw, rowh}, SET_CONSOL, 15);
    } else if (settings_get(SET_RULE) == 1) {
        setting_row(d, (az_rect){body.x, y + rowh + 8, colw, rowh}, SET_HCANY, 16);
        if (!settings_get(SET_HCANY)) setting_row(d, (az_rect){body.x, y + 2 * rowh + 16, colw, rowh}, SET_HCSLOT, 16);
    } else text(d, (az_rect){body.x, y + rowh + 12, colw, 44},
                "Next grid boundary in the outgoing track.", 13, dim());
    x = body.x + colw + gap;
    stacked_field(d, (az_rect){x, y, colw, rowh + 24}, SET_DECK);
    text(d, (az_rect){x, y + rowh + 38, colw, 24}, "LOOP / AUTOMATIC", 14, ink());
    text(d, (az_rect){x, y + rowh + 64, colw, 22}, "Anchor follows B's working loop, including while paused.", 12, dim());
    text(d, (az_rect){body.x, body.y + body.h - 44, body.w, 44},
         "Saved automatically. Use physical PLAY / loop EXIT.", 13, dim());
}
static void draw_compact(az_draw *d, az_rect a)
{
    int pad = 8, gap = 6;
    az_rect body = {a.x + pad, a.y + pad, a.w - 2 * pad, a.h - 2 * pad};
    const char *tabs[] = {"Guide", "Targets", "Incoming"};
    int tabw = (body.w - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) button(d, (az_rect){body.x + i * (tabw + gap), body.y, tabw, 44},
                                      tabs[i], -1, HIT_TAB, i, i == g_tab, 14);
    int y = body.y + 52, avail = body.h - 104;
    int group[SET_COUNT], n = 0;
    for (int i = 0; i < nfocus; i++) if (tab_for(focus_keys[i]) == g_tab) group[n++] = focus_keys[i];
    int sel = 0;
    for (int i = 0; i < n; i++) if (group[i] == g_sel) sel = i;
    /* The guide tab earns its space with visual placement buttons. */
    if (g_tab == 0 && avail >= 124) {
        setting_row(d, (az_rect){body.x, y, body.w, 44}, SET_SHOW, 15);
        placements(d, (az_rect){body.x, y + 50, body.w, 74});
        if (avail >= 244) sample_preview(d, (az_rect){body.x, y + 134, body.w, 110});
        g_visible = n;
        g_first = 0;
    } else {
        g_visible = avail / 52;
        if (g_visible < 1) g_visible = 1;
        if (g_visible > n) g_visible = n;
        if (g_first > n - g_visible) g_first = n - g_visible;
        if (g_first < 0) g_first = 0;
        if (sel < g_first) g_first = sel;
        if (sel >= g_first + g_visible) g_first = sel - g_visible + 1;
        for (int i = 0; i < g_visible; i++)
            setting_row(d, (az_rect){body.x, y + i * 52, body.w, 44}, group[g_first + i], 14);
    }
    page_controls(d, (az_rect){body.x, body.y + body.h - 44, body.w, 44});
}
static void draw_cb(az_draw *d, az_rect a, void *user)
{
    (void)user;
    nhits = 0;
    g_area = a;
    if (!C || !d || a.w < 240 || a.h < 180) return;
    rebuild_focus();
    C->fill_rect(d, a.x, a.y, a.w, a.h, bg());
    g_wide = a.w >= 900 && a.h >= 420;
    if (g_wide) draw_wide(d, a);
    else { g_tab = tab_for(g_sel); draw_compact(d, a); }
}
static void change(int key, int delta)
{
    if (!DESC[key].n) settings_set(key, !settings_get(key));
    else settings_set(key, settings_get(key) + delta);
}
static void move_focus(int dir)
{
    rebuild_focus();
    int idx = 0;
    while (idx < nfocus - 1 && focus_keys[idx] != g_sel) idx++;
    if (dir < 0 && idx > 0) idx--;
    if (dir > 0 && idx < nfocus - 1) idx++;
    g_sel = focus_keys[idx];
    g_tab = tab_for(g_sel);
}
static void move_page(int dir)
{
    int group[SET_COUNT], n = 0;
    rebuild_focus();
    for (int i = 0; i < nfocus; i++) if (tab_for(focus_keys[i]) == g_tab) group[n++] = focus_keys[i];
    int first = g_first + (dir < 0 ? -g_visible : g_visible);
    if (first >= 0 && first < n) { g_first = first; g_sel = group[first]; }
    else {
        g_tab = (g_tab + (dir < 0 ? 2 : 1)) % 3;
        g_sel = g_tab == 0 ? SET_SHOW : g_tab == 1 ? SET_RULE : SET_DECK;
        g_first = 0;
    }
}
static int input_cb(const az_event *ev, void *user)
{
    (void)user;
    if (!ev || !C || ev->kind == AZ_EV_BACK) return 0;
    if (g_area.w < 240 || g_area.h < 180) return 0;
    if (ev->kind == AZ_EV_KNOB) move_focus(ev->dir);
    else if (ev->kind == AZ_EV_CLICK) { rebuild_focus(); change(g_sel, 1); }
    else if (ev->kind == AZ_EV_TAP) {
        int found = 0;
        for (int i = 0; i < nhits; i++) {
            hit_t hit = hits[i];
            az_rect r = hit.r;
            if (ev->x < r.x || ev->y < r.y || ev->x >= r.x + r.w || ev->y >= r.y + r.h) continue;
            found = 1;
            if (hit.kind == HIT_TAB) {
                g_tab = hit.value;
                g_sel = g_tab == 0 ? SET_SHOW : g_tab == 1 ? SET_RULE : SET_DECK;
                g_first = 0;
            } else if (hit.kind == HIT_PAGE) {
                move_page(hit.value);
            } else {
                g_sel = hit.key;
                if (hit.kind == HIT_SET) settings_set(hit.key, hit.value);
                else change(hit.key, hit.value);
            }
            break;
        }
        if (!found) return 0;
    } else return 0;
    /* Do not let queued taps use regions from a different target type/page. */
    nhits = 0;
    rebuild_focus();
    C->request_redraw();
    return 1;
}
static void enter_cb(void *user)
{
    (void)user;
    g_sel = SET_SHOW; g_tab = g_first = nhits = 0;
    g_area = (az_rect){0};
}
static const az_screen_v1 settings_screen = {
    .size = sizeof(az_screen_v1),
    .on_draw = draw_cb,
    .on_input = input_cb,
    .on_enter = enter_cb,
    .user = NULL,
    .flags = AZ_SCREEN_DEDICATED,
};

