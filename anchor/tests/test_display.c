/* SPDX-License-Identifier: GPL-3.0-only
 * Verify action priority, deadline wording, actual deck numbers and all drawing bounds. */
#include "display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct az_draw { int unused; };
static az_rect area, boxes[32];
static int nboxes;
static int saw_bars, saw_beats, saw_loop;
static int image_calls, bar_font, beat_font;
static int tw(const char *s, int px) { return (int)strlen(s) * px * 3 / 5; }
static int intersects(az_rect a, az_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
static void bounds(int x, int y, int w, int h)
{
    assert(w > 0 && h > 0);
    assert(x >= area.x && y >= area.y && x + w <= area.x + area.w && y + h <= area.y + area.h);
}
static void text(az_draw *d, int x, int y, const char *s, int px, uint32_t col)
{
    (void)d;
    saw_bars |= col == AN_COLOR_BARS;
    saw_beats |= col == AN_COLOR_BEATS;
    saw_loop |= col == AN_COLOR_LOOPING;
    if (!strcmp(s,"4") && col==AN_COLOR_BARS) bar_font=px;
    if (s[0]=='.' && col==AN_COLOR_BARS) beat_font=px;
    assert(px >= 8 && px <= 96);
    az_rect r = {x, y, tw(s, px), px * 3 / 2};
    bounds(r.x, r.y, r.w, r.h);
    for (int i = 0; i < nboxes; i++) {
        if (intersects(r, boxes[i])) fprintf(stderr, "overlap %s (%d,%d %dx%d) with (%d,%d %dx%d)\n", s, r.x,r.y,r.w,r.h, boxes[i].x,boxes[i].y,boxes[i].w,boxes[i].h);
        assert(!intersects(r, boxes[i]));
    }
    assert(nboxes < 32);
    boxes[nboxes++] = r;
}
static void fill(az_draw *d, int x, int y, int w, int h, uint32_t col)
{ (void)d; (void)col; bounds(x, y, w, h); }
static void border(az_draw *d, int x, int y, int w, int h, int px, uint32_t col)
{ (void)px; fill(d, x, y, w, h, col); }

static int image_draw(az_draw *d, int x, int y, const unsigned char *rgba, int w, int h)
{
    (void)d; bounds(x,y,w,h); image_calls++;
    for (int i=0;i<w*h;i++) {
        assert(rgba[i*4+3]<=204);
        assert(rgba[i*4] <= rgba[i*4+3]);
    }
    assert(rgba[(w/2)*4+3]==204 && rgba[3]<204);
    return 0;
}

int main(void)
{
    an_plan_t p = {0};
    an_display_t m;
    p.valid = 1; p.state = AN_ST_SINGLE; p.accent = 1; p.bars_action = 4;
    snprintf(p.target, sizeof p.target, "CHORUS END");
    an_display_model(&p, 0, 1, &m);
    assert(m.count_unit == AN_COUNT_BARS);
    assert(!strcmp(m.ribbon, "PLAY DECK 2 IN 4.4"));
    assert(!strcmp(m.detail, "Target: Deck 1 CHORUS END"));
    p.bars_action = 0.75;
    an_display_model(&p, 1, 0, &m);
    assert(!strcmp(m.ribbon, "PLAY DECK 1 IN 1.3"));
    assert(m.count_unit == AN_COUNT_BEATS);
    p.bars_action = 0.01;
    an_display_model(&p, 1, 0, &m);
    assert(!strcmp(m.headline, "PLAY IN 1.1"));
    p.play_now = 1; p.accent = 4;
    an_display_model(&p, 0, 1, &m);
    assert(!strcmp(m.ribbon, "PLAY DECK 2 NOW"));
    p.tempo_mismatch = 1;
    an_display_model(&p, 0, 1, &m);
    assert(!strcmp(m.headline, "MATCH DECK TEMPOS"));
    assert(!m.number[0] && m.accent == 2);
    p.tempo_mismatch = p.play_now = 0;
    p.state = AN_ST_OPEN;
    an_display_model(&p, 0, 1, &m);
    assert(!strcmp(m.headline, "EXIT LOOP NOW"));
    p.state = AN_ST_WAIT; p.bars_action = 4;
    an_display_model(&p, 0, 1, &m);
    assert(!strcmp(m.headline, "EXIT IN 4.4"));
    p.state = AN_ST_NO_ANALYSIS; p.notice = AN_NOTICE_ANALYSIS_A;
    an_display_model(&p, 0, 1, &m);
    assert(!strcmp(m.deck, "DECK 1"));
    assert(!strcmp(m.ribbon, "DECK 1 - ANALYSE TRACK"));
    p.notice = AN_NOTICE_ANALYSIS_B;
    an_display_model(&p, 0, 1, &m);
    assert(!strcmp(m.deck, "DECK 2"));

    az_core_v1 c = {0};
    c.text_width = tw; c.draw_text = text; c.fill_rect = fill; c.draw_rect = border;
    az_draw d;
    area=an_display_rect(ANCHOR_LAYOUT_SIDEBAR);
    p=(an_plan_t){.valid=1,.state=AN_ST_WAIT,.accent=1,.bars_action=4,.looping=1};
    an_display_model(&p,0,1,&m); nboxes=saw_bars=saw_beats=saw_loop=0;
    an_display_draw(&c,&d,area,ANCHOR_LAYOUT_SIDEBAR,&m);
    assert(saw_bars && !saw_beats && saw_loop);
    assert(bar_font==96 && beat_font>0 && beat_font<bar_font);
    p.bars_action=.75;
    an_display_model(&p,0,1,&m); nboxes=saw_bars=saw_beats=saw_loop=0;
    an_display_draw(&c,&d,area,ANCHOR_LAYOUT_SIDEBAR,&m);
    assert(!saw_bars && saw_beats && saw_loop);
    assert(strstr(m.deck,"LOOP"));
    p=(an_plan_t){.valid=1,.state=AN_ST_NEED_TRACKS};
    an_display_model(&p,-1,-1,&m); assert(!strcmp(m.headline,"LOAD TWO TRACKS"));
    c.size=sizeof c; c.draw_image=image_draw;
    area=an_sidebar_tab_rect(0); nboxes=0; an_sidebar_tab_draw(&c,&d,area,0);
    assert(nboxes==1); /* closed: triangle + label */
    area=an_sidebar_tab_rect(1); nboxes=0; an_sidebar_tab_draw(&c,&d,area,1);
    assert(nboxes==0); /* open: triangle only, no duplicate label */
    assert(image_calls==2);
    for(int beat=4;beat>=1;beat--) {
        p=(an_plan_t){.state=AN_ST_SINGLE,.bars_action=beat/4.0};
        an_display_model(&p,0,1,&m);
        char expected[8];snprintf(expected,sizeof expected,".%d",beat);
        assert(!strcmp(m.number,"1") && !strcmp(m.beat,expected));
        assert(m.count_unit==AN_COUNT_BEATS);
    }
    for (int layout = 1; layout <= 3; layout++) {
        for (int state = AN_ST_NONE; state <= AN_ST_MISSED; state++) {
            for (int notice = AN_NOTICE_NONE; notice <= AN_NOTICE_START_OUTGOING; notice++) {
                p.state = state; p.notice = notice;
                snprintf(p.line2, sizeof p.line2, "Long diagnostic message to verify it stays inside its own secondary text area");
                for (int variant = 0; variant < 6; variant++) {
                    p.bars_action = variant == 0 ? 0.25 : variant == 1 ? 4 : 128;
                    p.play_now = variant == 3;
                    p.tempo_mismatch = variant == 4;
                    p.loop_prepared = variant == 5;
                    an_display_model(&p, 0, 1, &m);
                    const az_rect sizes[] = {an_display_rect(layout), {13, 17, 80, 24},
                                            {13, 17, 180, 120}, {13, 17, 500, 100}};
                    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
                        area = sizes[i]; nboxes = 0;
                        an_display_draw(&c, &d, area, layout, &m);
                        assert(nboxes > 0);
                    }
                }
            }
        }
    }
    puts("display tests passed: action priority, deck roles, countdowns, bounds and text overlap");
    return 0;
}
