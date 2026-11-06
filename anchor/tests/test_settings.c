/* Functional and geometry checks for production settings paint/input code. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "azmod-mod.h"
struct az_draw { int unused; };
#include "../settings.c"

static az_rect bounds;
static az_rect texts[256];
static int ntexts, saved[SET_COUNT], redraws, setting_events;
static void setting_changed(void) { setting_events++; }
static int text_width(const char *s, int px) { return (int)(strlen(s) * px * .66 + .5); }
static void rect_check(int x, int y, int w, int h)
{
    assert(w >= 0 && h >= 0);
    assert(x >= bounds.x && y >= bounds.y);
    assert(x + w <= bounds.x + bounds.w && y + h <= bounds.y + bounds.h);
}
static void fill(az_draw *d, int x, int y, int w, int h, uint32_t col)
{ (void)d; (void)col; rect_check(x,y,w,h); }
static void rect(az_draw *d, int x, int y, int w, int h, int thick, uint32_t col)
{ (void)thick; fill(d,x,y,w,h,col); }
static void draw_string(az_draw *d, int x, int y, const char *s, int px, uint32_t col)
{
    (void)d; (void)col;
    az_rect r = {x,y,text_width(s,px),px * 3 / 2};
    rect_check(r.x,r.y,r.w,r.h);
    for (int i = 0; i < ntexts; i++) {
        az_rect b = texts[i];
        assert(!(r.x < b.x+b.w && b.x < r.x+r.w && r.y < b.y+b.h && b.y < r.y+r.h));
    }
    assert(ntexts < 256); texts[ntexts++] = r;
}
static uint32_t color(int role) { return role == AZ_COL_BG ? 0x181818 : role == AZ_COL_HI ? 0x66ddee : role == AZ_COL_HI_TEXT ? 0x101010 : 0xf0f0f0; }
static int get(const char *id, const char *key, int def)
{
    (void)id;
    for (int i = 0; i < SET_COUNT; i++) if (!strcmp(key,KEYS[i])) return saved[i];
    return def;
}
static void set(const char *id, const char *key, int value)
{ (void)id; for (int i = 0; i < SET_COUNT; i++) if (!strcmp(key,KEYS[i])) saved[i] = value; }
static void redraw(void) { redraws++; }
static az_draw draw;
static void paint(int w, int h)
{
    bounds = (az_rect){37,81,w,h}; ntexts = 0;
    settings_screen.on_draw(&draw,bounds,NULL);
    for (int i = 0; i < nhits; i++) {
        rect_check(hits[i].r.x,hits[i].r.y,hits[i].r.w,hits[i].r.h);
        assert(hits[i].r.w >= 44 && hits[i].r.h >= 44);
        for (int j = 0; j < i; j++) {
            az_rect a = hits[i].r, b = hits[j].r;
            assert(!(a.x < b.x+b.w && b.x < a.x+a.w && a.y < b.y+b.h && b.y < a.y+a.h));
        }
    }
}
static void event(int kind, int dir)
{
    az_event e = {.kind=kind,.dir=dir};
    assert(settings_screen.on_input(&e,NULL));
}
static void press_hit(int kind, int key, int value)
{
    for (int i = 0; i < nhits; i++) if (hits[i].kind == kind && hits[i].key == key && hits[i].value == value) {
        az_rect r = hits[i].r;
        az_event e = {.kind=AZ_EV_TAP,.x=r.x+r.w/2,.y=r.y+r.h/2};
        assert(settings_screen.on_input(&e,NULL)); return;
    }
    assert(!"expected control is missing");
}
int main(void)
{
    az_core_v1 core = {.size=sizeof core,.draw_text=draw_string,.fill_rect=fill,.draw_rect=rect,
                      .color=color,.text_width=text_width,.cfg_get=get,.cfg_set=set,.request_redraw=redraw};
    for (int i = 0; i < SET_COUNT; i++) saved[i] = DESC[i].def;
    settings_init(&core, setting_changed); settings_screen.on_enter(NULL);
    paint(1280,420);
    press_hit(HIT_SET,SET_DISPLAY,0); assert(settings_get(SET_DISPLAY) == 0);
    assert(setting_events == 1);
    assert(nhits == 0); /* old hit regions cannot act after a layout change */
    paint(1280,420); press_hit(HIT_SET,SET_DISPLAY,0); assert(settings_get(SET_DISPLAY) == 0 && setting_events == 1);
    paint(1280,420); press_hit(HIT_SET,SET_DISPLAY,2);
    settings_init(&core, setting_changed); assert(settings_get(SET_DISPLAY) == 2);
    paint(1280,420); press_hit(HIT_CYCLE,SET_RULE,1); assert(settings_get(SET_RULE) == 1);
    paint(1280,420);
    for (int i = 0; i < nhits; i++) assert(hits[i].key != SET_PHRASE && hits[i].key != SET_POINT && hits[i].key != SET_HCSLOT);
    press_hit(HIT_CYCLE,SET_HCANY,1); paint(1280,420);
    press_hit(HIT_CYCLE,SET_HCSLOT,1); assert(settings_get(SET_HCSLOT) == 1);
    settings_set(SET_RULE,0); settings_screen.on_enter(NULL); paint(480,240);
    press_hit(HIT_TAB,-1,1); paint(480,240); assert(g_sel == SET_RULE);
    event(AZ_EV_KNOB,1); paint(480,240); assert(g_sel == SET_PHRASE);
    event(AZ_EV_CLICK,0); assert(settings_get(SET_PHRASE) == 1);
    unsigned seen = 0;
    settings_screen.on_enter(NULL);
    for (int i = 0; i < SET_COUNT + 3; i++) {
        paint(480,240); seen |= 1u << g_sel; event(AZ_EV_KNOB,1);
    }
    assert((seen & ((1u << SET_SHOW)|(1u << SET_DISPLAY)|(1u << SET_RULE)|(1u << SET_PHRASE)|
            (1u << SET_POINT)|(1u << SET_CONSOL)|(1u << SET_DECK))) == seen);
    assert(seen & (1u << SET_DECK));
    unsigned touched = 0;
    settings_screen.on_enter(NULL);
    for (int i = 0; i < 12; i++) {
        paint(480,240);
        for (int j = 0; j < nhits; j++) if (hits[j].key >= 0) touched |= 1u << hits[j].key;
        press_hit(HIT_PAGE,-1,1);
    }
    assert((touched & seen) == seen); /* every knob-accessible setting also has a touch path */
    az_event back = {.kind=AZ_EV_BACK}; assert(!settings_screen.on_input(&back,NULL));
    /* Non-zero origins, long labels, conditional fields, every placement, restricted sizes. */
    const int sizes[][2] = {{1280,800},{1280,420},{1024,420},{900,420},{899,420},
                            {800,300},{480,240},{320,420},{240,180},{120,80}};
    for (unsigned z = 0; z < sizeof sizes/sizeof sizes[0]; z++)
        for (int mode = 0; mode < 4; mode++) for (int layout = 0; layout < 3; layout++) {
            settings_set(SET_RULE,mode); settings_set(SET_DISPLAY,layout);
            settings_set(SET_PHRASE,4); settings_set(SET_HCANY,0);
            settings_set(SET_HCSLOT,7); settings_set(SET_DECK,2);
            settings_screen.on_enter(NULL);
            for (int i = 0; i < SET_COUNT; i++) { paint(sizes[z][0],sizes[z][1]); if(nhits)event(AZ_EV_KNOB,1); }
        }
    saved[SET_RULE]=999; settings_init(&core, setting_changed); assert(settings_get(SET_RULE)==0);
    assert(redraws > 0);
    puts("Settings passed: touch bounds/overlap, conditional controls, paging, knob, persistence and setting events");
}
