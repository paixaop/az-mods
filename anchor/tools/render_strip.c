/* render_strip: draw Anchor's deck strip and target selector with a stub core into a JSON
 * draw list, for layout review on a computer. Development tool, not part of the mod. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "azmod-mod.h"
#include "mod.h"

struct az_draw {
    int dummy;
};

typedef struct {
    int kind;
    int x, y, w, h, px;
    uint32_t col;
    char s[256];
} call_t;
static call_t calls[4096];
static int ncalls;
static int first = 1;
static az_rect bounds_area;
static az_rect placed_area;
static az_rect placed_tab;
static az_surface *test_tab;
static int saved_display = -1;

static int text_width(const char *s, int px)
{
    if (!s) return 0;
    return (int)(0.59976 * px * (double)strlen(s) + 0.5);
}
static void rec(call_t *c)
{
    assert(c->x >= bounds_area.x && c->y >= bounds_area.y);
    assert(c->x + c->w <= bounds_area.x + bounds_area.w);
    assert(c->y + c->h <= bounds_area.y + bounds_area.h);
    if (!c->kind) {
        assert(c->px >= 8 && c->px <= 96);
        for (int i = 0; i < ncalls; i++) {
            const call_t *b = &calls[i];
            if (!b->kind) assert(!(c->x < b->x + b->w && b->x < c->x + c->w &&
                                    c->y < b->y + b->h && b->y < c->y + c->h));
        }
    }
    if (ncalls < (int)(sizeof calls / sizeof calls[0])) calls[ncalls++] = *c;
}
static void draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t rgb)
{
    (void)d;
    call_t c;
    memset(&c, 0, sizeof c);
    c.kind = 0, c.x = x, c.y = y, c.px = px, c.col = rgb, c.w = text_width(s, px), c.h = px * 3 / 2;
    snprintf(c.s, sizeof c.s, "%s", s ? s : "");
    rec(&c);
}
static void fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t rgb)
{
    (void)d;
    call_t c;
    memset(&c, 0, sizeof c);
    c.kind = 1, c.x = x, c.y = y, c.w = w, c.h = h, c.col = rgb;
    rec(&c);
}
static void draw_rect(az_draw *d, int x, int y, int w, int h, int width, uint32_t rgb)
{
    (void)d;
    for (int i = 0; i < width; i++) {
        call_t c;
        memset(&c, 0, sizeof c);
        c.kind = 2, c.x = x + i, c.y = y + i, c.w = w - 2 * i, c.h = h - 2 * i, c.col = rgb;
        if (c.w > 0 && c.h > 0) rec(&c);
    }
}
static uint32_t color(int role)
{
    switch (role) {
    case AZ_COL_TEXT: return 0xf2f2f2;
    case AZ_COL_TEXT_DIM: return 0x9a9a9a;
    case AZ_COL_HI: return 0xff8a00;
    case AZ_COL_HI_TEXT: return 0x101010;
    case AZ_COL_BG: return 0x181818;
    default: return 0xffffff;
    }
}
static void request_redraw(void) {}
static void surface_redraw(az_surface *s) { (void)s; }
static void surface_place(az_surface *s, az_rect r) { if(s==test_tab)placed_tab=r;else placed_area=r; }
static void surface_show(az_surface *s, int on) { (void)s; (void)on; }
static int draw_image(az_draw *d, int x, int y, const unsigned char *rgba, int w, int h)
{
    (void)rgba;
    fill_rect(d, x, y, w, h, 0x282828);
    return 0;
}
static int cfg_get(const char *id, const char *key, int def)
{
    assert(!strcmp(id, ANCHOR_MOD_ID));
    return !strcmp(key, "display") && saved_display >= 0 ? saved_display : def;
}
static void cfg_set(const char *id, const char *key, int v)
{
    assert(!strcmp(id, ANCHOR_MOD_ID));
    if (!strcmp(key, "display")) saved_display = v;
}

/* Anchor itself. */
#include "anchor.c"

static void scene(const char *name)
{
    settings_set(SET_SHOW, 1);
    ncalls = 0;
    sel_open = sel_b = sel_pending = 0;
    cur_a = 0; cur_b = 1;
    sel_deck = cur_a;
    decks[0].sample_rate = decks[1].sample_rate = 48000; /* preview fixture clock */
    memset(&plan, 0, sizeof plan);
    plan.valid = 1;
    plan.state = AN_ST_SINGLE;
    plan.accent = 1;
    plan.bars_action = 4;
    plan.b_target_valid = 1; plan.b_target_ms = 134000;
    snprintf(plan.b_target, sizeof plan.b_target, "CHORUS START");
    snprintf(plan.target, sizeof plan.target, "CHORUS END");
    snprintf(plan.line1, sizeof plan.line1, "CHORUS END - 16 BARS");
    snprintf(plan.line2, sizeof plan.line2, "PLAY IN 4 BARS - 2 LOOPS (SET)");
    if (!strcmp(name, "plan") || !strcmp(name, "exit")) {
        plan.state = AN_ST_OPEN; plan.accent = 4; plan.window_open = 1; plan.looping = 1;
    } else if (!strcmp(name, "now")) {
        plan.play_now = 1; plan.accent = 4; plan.bars_action = 0;
    } else if (!strcmp(name, "beats")) {
        plan.bars_action = 0.75;
    } else if (!strcmp(name, "prepared")) {
        plan.state=AN_ST_START;plan.loop_prepared=1;plan.loops=3;
    } else if (!strcmp(name, "wait")) {
        plan.state = AN_ST_WAIT; plan.looping = 1;
    } else if (!strcmp(name, "looping")) {
        plan.state = AN_ST_WAIT; plan.looping = 1;
    } else if (!strcmp(name, "needtracks")) {
        plan.state = AN_ST_NEED_TRACKS; cur_a = cur_b = -1;
    } else if (!strcmp(name, "ready")) {
        plan.notice = AN_NOTICE_START_OUTGOING; cur_a = cur_b = -1;
    } else if (!strcmp(name, "reading-loop")) {
        plan.state = AN_ST_OFF; plan.notice = AN_NOTICE_LOOP_UNKNOWN;
    } else if (!strcmp(name, "released")) {
        plan.state = AN_ST_RELEASED; plan.accent = 4;
    } else if (!strcmp(name, "missed")) {
        plan.state = AN_ST_MISSED; plan.accent = 3;
    } else if (!strcmp(name, "reading")) {
        plan.state = AN_ST_NO_ANALYSIS; plan.notice = AN_NOTICE_READING;
        plan.target[0] = 0;
    } else if (!strcmp(name, "tempo")) {
        plan.tempo_mismatch = 1; plan.accent = 2;
    } else if (!strcmp(name, "selector") || !strcmp(name, "b-selector") || !strcmp(name, "empty")) {
        sel_open = 1;
        sel_page = 0;
        sel_n = !strcmp(name, "empty") ? 0 : 7;
        static const char *labels[] = { "CHORUS END", "OUTRO START", "HC3", "BRIDGE END",
                                        "CHORUS START", "BREAK END", "HC6" };
        static const unsigned when[] = { 195000, 240000, 270000, 300000, 330000, 360000, 390000 };
        for (int i = 0; i < sel_n; i++) {
            memset(&sel_cand[i], 0, sizeof sel_cand[i]);
            snprintf(sel_cand[i].label, sizeof sel_cand[i].label, "%s", labels[i]);
            sel_cand[i].ms = when[i];
        }
        latched_ms = when[0];
        if (!strcmp(name, "b-selector")) {
            sel_b = 1; sel_kind = 0; sel_deck = cur_b;
            static const char *starts[] = {"BUILD START", "CHORUS START", "BREAK START", "VERSE START",
                                          "CHORUS START", "OUTRO START", "BRIDGE START"};
            for (int i=0;i<sel_n;i++) snprintf(sel_cand[i].label,sizeof sel_cand[i].label,"%s",starts[i]);
            b_targets[cur_b].target = sel_cand[0];
            b_targets[cur_b].valid = 1;
        }
    }
}

static int overlap(az_rect a, az_rect b)
{ return a.x < b.x+b.w && b.x < a.x+a.w && a.y < b.y+b.h && b.y < a.y+a.h; }

static void verify_sidebar(az_draw *d)
{
    static int fake_body, fake_tab;
    strip=(az_surface *)&fake_body; sidebar_tab=(az_surface *)&fake_tab;
    test_tab=sidebar_tab;
    cur_layout = 0;
    settings_set(SET_SHOW, 1);
    settings_set(SET_DISPLAY, 2);
    update_guide(mono_ms());
    assert(tab_shown && strip_shown && plan.state==AN_ST_NEED_TRACKS);
    bounds_area=an_display_rect(ANCHOR_LAYOUT_SIDEBAR); ncalls=0; paint(d,bounds_area,NULL);
    an_display_t m; an_display_model(&plan,-1,-1,&m); assert(!strcmp(m.headline,"LOAD TWO TRACKS"));
    az_rect tab=an_sidebar_tab_rect(1), closed=an_sidebar_tab_rect(0), fx={1195,238,85,46};
    assert(tab.y >= fx.y+fx.h+8 && !overlap(tab,fx) && !overlap(closed,fx));
    assert(overlap(bounds_area,fx) && bounds_area.x+bounds_area.w==1280);
    assert(placed_tab.x+placed_tab.w+AN_SIDEBAR_HANDLE_GAP==placed_area.x); /* open close handle sits on the left */
    az_event tap_tab={.kind=AZ_EV_TAP,.x=tab.x+tab.w/2,.y=tab.y+tab.h/2};
    assert(tab_input(&tap_tab,NULL)); assert(!settings_get(SET_SHOW));
    assert(!strip_shown && tab_shown);
    assert(!memcmp(&placed_tab,&closed,sizeof closed));
    tap_tab.x=closed.x+closed.w/2; tap_tab.y=closed.y+closed.h/2;
    assert(tab_input(&tap_tab,NULL));
    assert(settings_get(SET_SHOW) && strip_shown && tab_shown);
    tap_tab.x=fx.x+10; tap_tab.y=fx.y+10;
    assert(!tab_input(&tap_tab,NULL)); /* native Beat FX tap is never consumed */
    strip=sidebar_tab=NULL;
    puts("Sidebar passed: empty-track help, tab separation, event-driven hide and reopen");
}

static void tap(az_rect r)
{
    az_event ev = {.kind = AZ_EV_TAP, .x = r.x + r.w / 2, .y = r.y + r.h / 2};
    assert(input(&ev, NULL) == 1);
}

static int same_source_ms(uint32_t a, uint32_t b)
{
    int order = compare_timestamps(a, b, 1000, 48000, 1); /* preview fixture clock */
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL;
}

static void verify(az_draw *d)
{
    const char *states[] = {"play", "now", "beats", "prepared", "wait", "exit", "released", "missed",
                            "reading", "tempo", "selector", "b-selector", "empty"};
    for (unsigned i = 0; i < sizeof states / sizeof states[0]; i++) {
        scene(states[i]);
        paint(d, bounds_area, NULL);
        assert(ncalls > 0);
    }
    scene("selector"); paint(d, bounds_area, NULL);
    int per = sel_per;
    tap(arrow_r_r);
    assert(sel_page == 1);
    ncalls = 0; paint(d, bounds_area, NULL);
    uint32_t expected = sel_cand[per].ms;
    tap(chip_r[0]);
    assert(!sel_open && same_source_ms(latched_ms, expected));
    scene("selector"); paint(d, bounds_area, NULL);
    tap(close_r); assert(!sel_open);
    scene("selector"); paint(d, bounds_area, NULL);
    az_event back = {.kind = AZ_EV_BACK};
    assert(input(&back, NULL) == 1 && !sel_open);
    scene("selector");
    az_rect normal = bounds_area;
    bounds_area = (az_rect){0, 0, 100, 30};
    paint(d, bounds_area, NULL);
    assert(!chip_r[0].w && !close_r.w);
    bounds_area = normal;

    saved_display = -1;
    settings_init(C, settings_changed);
    assert(settings_get(SET_DISPLAY) == 1); /* Card is the default. */
    static int fake_surface;
    strip = (az_surface *)&fake_surface;
    cur_layout = 0;
    update_guide(mono_ms());
    for (int option = 0; option < 3; option++) {
        settings_set(SET_DISPLAY, (option + 1) % 3);
        settings_set(SET_DISPLAY, option);
        az_rect expected_area = an_display_rect(option + 1);
        assert(cur_layout == option + 1);
        assert(!memcmp(&placed_area, &expected_area, sizeof placed_area));
    }
    settings_init(C, settings_changed);
    assert(settings_get(SET_DISPLAY) == 2 && selected_layout() == ANCHOR_LAYOUT_SIDEBAR);
    settings_set(SET_SHOW, 0);
    assert(settings_get(SET_SHOW) == 0);
    strip = NULL;
    puts("UI integration passed: all states, text bounds/overlap, target picker and live display switching");
}

static void verify_b_controls(az_draw *d, int layout)
{
    cur_layout=layout; bounds_area=an_display_rect(layout); bounds_area.x=bounds_area.y=0;
    scene("play");
    const an_phrase_t phrases[] = {{0, 8000, AN_CAT_BUILD, 0}, {8000, 16000, AN_CAT_CHORUS, 1}};
    const an_cue_t cues[] = {{.slot = 3, .time_ms = 6000}};
    decks[1] = (an_deck_status_t){
        .loaded = 1, .position_valid = 1, .pos_ms = 3217, .sample_rate = 48000,
        .analysis = {.phrases = phrases, .nphrases = 2, .cues = cues, .ncues = 1},
    };
    paint(d,bounds_area,NULL);
    assert(controls_ok && target_controls[1].w);
    assert(target_controls[0].h>=36 && target_controls[1].h>=36);
    for(int i=0;i<ncalls;i++) assert(!strstr(calls[i].s,"JUMP"));
    tap(target_controls[1]);assert(sel_open && sel_b && sel_n==2);
    ncalls=0;paint(d,bounds_area,NULL);
    int saw_set_here=0;
    for(int i=0;i<ncalls;i++) if(!calls[i].kind && !strcmp(calls[i].s,"SET HERE")) saw_set_here=1;
    assert(saw_set_here); /* narrow sidebar must retain the full action label */
    tap(chip_r[1]); assert(!sel_open && same_source_ms(b_targets[1].target.ms, 8000));
    ncalls=0;paint(d,bounds_area,NULL);tap(target_controls[1]);
    ncalls=0;paint(d,bounds_area,NULL);tap(kind_r[1]);
    assert(sel_kind==1 && sel_n==1);
    ncalls=0;paint(d,bounds_area,NULL);tap(chip_r[0]);
    assert(same_source_ms(b_targets[1].target.ms, 6000));
    ncalls=0;paint(d,bounds_area,NULL);tap(target_controls[1]);
    ncalls=0;paint(d,bounds_area,NULL);tap(kind_r[2]);
    assert(!sel_open && same_source_ms(b_targets[1].target.ms, 3217));
    memset(decks, 0, sizeof decks);
    puts("B UI passed: separate A/B controls, phrase/Hot Cue/Set here taps");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s out.json play|now|beats|wait|exit|released|missed|reading|tempo|selector|empty|verify [ribbon|card|sidebar]\n", argv[0]);
        return 2;
    }
    static az_core_v1 core;
    core.size = sizeof core;
    core.draw_text = draw_text; core.fill_rect = fill_rect; core.draw_rect = draw_rect; core.draw_image = draw_image;
    core.color = color; core.text_width = text_width; core.request_redraw = request_redraw;
    core.surface_redraw = surface_redraw; core.cfg_get = cfg_get; core.cfg_set = cfg_set;
    core.surface_place = surface_place; core.surface_show = surface_show;
    C = &core;
    az_draw d;
    settings_init(C, settings_changed);
    cur_layout = ANCHOR_LAYOUT_CARD;
    if (argc > 3) {
        if (!strcmp(argv[3], "ribbon")) cur_layout = ANCHOR_LAYOUT_RIBBON;
        else if (!strcmp(argv[3], "card")) cur_layout = ANCHOR_LAYOUT_CARD;
        else if (!strcmp(argv[3], "sidebar")) cur_layout = ANCHOR_LAYOUT_SIDEBAR;
        else { fprintf(stderr, "unknown display: %s\n", argv[3]); return 2; }
    }
    if (!strcmp(argv[2], "verify")) {
        for (int layout = 1; layout <= 3; layout++) {
            cur_layout = layout;
            bounds_area = an_display_rect(layout);
            bounds_area.x = bounds_area.y = 0;
            verify(&d);
            verify_b_controls(&d,layout);
        }
        verify_sidebar(&d);
        return 0;
    }
    bounds_area = an_display_rect(cur_layout);
    bounds_area.x = bounds_area.y = 0;
    scene(argv[2]);
    if (!strcmp(argv[2],"tab") || !strcmp(argv[2],"hidden-tab")) {
        bounds_area=an_sidebar_tab_rect(!strcmp(argv[2],"tab")); bounds_area.x=bounds_area.y=0;
        an_sidebar_tab_draw(C,&d,bounds_area,!strcmp(argv[2],"tab"));
    } else paint(&d, bounds_area, NULL);

    FILE *out = fopen(argv[1], "w");
    if (!out) return 1;
    fprintf(out, "{\"w\":%d,\"h\":%d,\"calls\":[", bounds_area.w, bounds_area.h);
    for (int i = 0; i < ncalls; i++) {
        call_t *c = &calls[i];
        if (!first) fputc(',', out);
        first = 0;
        fprintf(out, "{\"k\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"px\":%d,\"col\":%u,\"s\":\"",
                c->kind, c->x, c->y, c->w, c->h, c->px, c->col);
        for (const char *p = c->s; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', out);
            fputc(*p, out);
        }
        fprintf(out, "\"}");
    }
    fprintf(out, "]}\n");
    fclose(out);
    return 0;
}
