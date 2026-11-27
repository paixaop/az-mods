/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only crop of the production SDK toolbar/picker paint callbacks. This
 * neutral canvas verifies widget geometry, not the firmware beneath it. */
#include "cues.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct az_draw {
    int unused;
};
#include "../cues.c"
static FILE *output;
static int first = 1, day;
static cues_config preview_config;
const az_screen_v1 *cues_settings_init(const az_core_v1 *core, void (*changed)(void))
{
    (void)core;
    (void)changed;
    return NULL;
}
void cues_settings_start_storage(void)
{
}
const cues_config *cues_settings_config(void)
{
    return &preview_config;
}
void cues_settings_status(const char *s)
{
    (void)s;
}
void cues_settings_tick(void)
{
}
int cues_build_plan(const cues_config *c, const az_track_snapshot *t,
                    const az_native_cue_snapshot *e, uint32_t rate, cues_plan *out)
{
    (void)c;
    (void)t;
    (void)e;
    (void)rate;
    *out = (cues_plan){0};
    return AZ_OK;
}
static void quoted(const char *s)
{
    fputc('"', output);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            fprintf(output, "\\%c", *p);
        else if (*p < 32)
            fprintf(output, "\\u%04x", *p);
        else
            fputc(*p, output);
    }
    fputc('"', output);
}
static int tw(const char *s, int px)
{
    return (int)(strlen(s) * px * .66 + .5);
}
static void record(int kind, int x, int y, int w, int h, int px, uint32_t rgb, const char *text)
{
    x -= 850;
    assert(x >= 0 && y >= 0 && x + w <= 280 && y + h <= 264);
    fprintf(output, "%s{\"k\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"px\":%d,\"col\":%u,\"s\":",
            first ? "" : ",", kind, x, y, w, h, px, rgb);
    quoted(text);
    fputc('}', output);
    first = 0;
}
static void dt(az_draw *d, int x, int y, const char *s, int px, uint32_t rgb)
{
    (void)d;
    record(0, x, y, tw(s, px), px * 3 / 2, px, rgb, s);
}
static void fr(az_draw *d, int x, int y, int w, int h, uint32_t rgb)
{
    (void)d;
    record(1, x, y, w, h, 0, rgb, "");
}
static void dr(az_draw *d, int x, int y, int w, int h, int thick, uint32_t rgb)
{
    (void)d;
    for (int i = 0; i < thick; i++)
        record(2, x + i, y + i, w - 2 * i, h - 2 * i, 0, rgb, "");
}
static void surface_redraw_stub(az_surface *surface) { (void)surface; }
static uint32_t palette(int role)
{
    static const uint32_t dark[] = {0xf2f2f2, 0x9a9a9a, 0xff8a00, 0x101010, 0x181818};
    static const uint32_t light[] = {0x17232b, 0x52616e, 0x007b91, 0xffffff, 0xf2f5f7};
    return day ? light[role] : dark[role];
}
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.json [day]\n", argv[0]);
        return 2;
    }
    day = argc > 2 && !strcmp(argv[2], "day");
    static az_core_v1 core = {.size = sizeof core,
                              .abi = AZ_MOD_ABI,
                              .draw_text = dt,
                              .fill_rect = fr,
                              .draw_rect = dr,
                              .color = palette,
                              .text_width = tw,
                              .surface_redraw = surface_redraw_stub};
    C = &core;
    decks[0].loaded = decks[1].loaded = decks[3].loaded = 1;
    picker_open = 1;
    az_event knob = {.kind = AZ_EV_KNOB, .dir = 1};
    picker_input(&knob, NULL);
    output = fopen(argv[1], "w");
    if (!output)
        return 1;
    fprintf(output, "{\"w\":280,\"h\":264,\"calls\":[");
    az_draw draw;
    button_paint(&draw, (az_rect){866, 4, 76, 44}, NULL);
    picker_paint(&draw, (az_rect){866, 56, 244, 196}, NULL);
    fprintf(output, "]}\n");
    fclose(output);
    return 0;
}
