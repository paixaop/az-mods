/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only preview of production draw/input code. No extra mod exports. */
#include "cues.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct az_draw {
    int unused;
};
#include "../settings.c"
static FILE *output;
static int first = 1, day;
int cues_store_init(const az_core_v1 *core, cues_config *out, char *status, size_t n)
{
    (void)core;
    cues_config_default(out);
    snprintf(status, n, "Rules saved to USB");
    return 1;
}
int cues_store_start(char *status, size_t n)
{
    (void)status;
    (void)n;
    return 1;
}
int cues_store_request(const cues_config *c, unsigned rev)
{
    (void)c;
    (void)rev;
    return 0;
}
int cues_store_take(unsigned *rev, char *status, size_t n)
{
    (void)rev;
    (void)status;
    (void)n;
    return 0;
}
static int tw(const char *s, int px)
{
    return (int)(strlen(s) * px * .66 + .5);
}
static void string(const char *s)
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
static void record(int kind, int x, int y, int w, int h, int px, uint32_t rgb, const char *text)
{
    fprintf(output, "%s{\"k\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"px\":%d,\"col\":%u,\"s\":",
            first ? "" : ",", kind, x, y, w, h, px, rgb);
    string(text);
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
    (void)thick;
    record(2, x, y, w, h, 0, rgb, "");
}
static uint32_t palette(int role)
{
    static const uint32_t dark[] = {0xf2f2f2, 0x9a9a9a, 0xff8a00, 0x101010, 0x181818};
    static const uint32_t light[] = {0x17232b, 0x52616e, 0x007b91, 0xffffff, 0xf2f5f7};
    return day ? light[role] : dark[role];
}
static void rr(void)
{
}
int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s out.json width height "
                "groups|group|rules|phrase|absolute|all-pads|memory-pad|choices|global|files|help [day]\n",
                argv[0]);
        return 2;
    }
    int w = atoi(argv[2]), h = atoi(argv[3]);
    day = argc > 5 && !strcmp(argv[5], "day");
    az_core_v1 core = {.size = sizeof core,
                       .abi = AZ_MOD_ABI,
                       .draw_text = dt,
                       .fill_rect = fr,
                       .draw_rect = dr,
                       .color = palette,
                       .text_width = tw,
                       .request_redraw = rr};
    cues_settings_init(&core, NULL);
    cues_settings_start_storage();
    config.group_count = 2;
    for (int i = 0; i < 2; i++) {
        cues_group *g = &config.groups[i];
        cues_group_default(g, &config);
        snprintf(g->name, sizeof g->name, "%s", i ? "Deep house" : "All tracks");
        g->rule_count = i ? 2 : 3;
        for (int j = 0; j < g->rule_count; j++) {
            cues_rule_default(&g->rules[j], g);
            g->rules[j].pad = j + 1;
            snprintf(g->rules[j].name, sizeof g->rules[j].name, "%s",
                     j == 0   ? "{phrase} in"
                     : j == 1 ? "Drop"
                              : "Outro loop");
        }
    }
    config.enabled = 1;
    active_config = config;
    snprintf(runtime_status, sizeof runtime_status,
             "Track load: waits for phrases; skips tracks with cues");
    const char *mode = argv[4];
    cues_group *g = &config.groups[0];
    if (!strcmp(mode, "group"))
        view = GROUP;
    else if (!strcmp(mode, "rules")) {
        view = RULES;
        g->rules[0].anchor = CUES_ABSOLUTE;
        g->rules[0].unit = CUES_SECONDS;
        g->rules[0].color = CUES_COLOR_BY_PAD;
        snprintf(g->rules[0].name, sizeof g->rules[0].name, "Track start");
        g->rules[1].phrase_type = 4;
        g->rules[1].offset = -16;
        g->rules[1].loop_mode = CUES_LOOP_TO_END;
    } else if (!strcmp(mode, "phrase")) {
        g->rules[0].phrase_type = 4;
        g->rules[0].phrase_index = 2;
        g->rules[0].offset = -16;
        g->rules[0].loop_mode = CUES_LOOP_TO_END;
        open_rule(g, 0);
    } else if (!strcmp(mode, "all-pads") || !strcmp(mode, "memory-pad")) {
        g->rules[0].phrase_type = 4;
        g->rules[0].phrase_index = 0;
        if (!strcmp(mode, "memory-pad"))
            g->rules[0].cue_type = AZ_CUE_MEMORY;
        open_rule(g, 0);
    } else if (!strcmp(mode, "absolute")) {
        g->rules[0].anchor = CUES_ABSOLUTE;
        g->rules[0].cue_type = AZ_CUE_MEMORY;
        g->rules[0].position = 32;
        open_rule(g, 0);
    } else if (!strcmp(mode, "choices")) {
        open_rule(g, 0);
        rebuild();
        for (int i = 0; i < field_count; i++)
            if (fields[i].value == &rule_edit.color) change_field(i);
    } else if (!strcmp(mode, "global"))
        view = GLOBAL;
    else if (!strcmp(mode, "files"))
        view = FILES;
    else if (!strcmp(mode, "help"))
        open_help();
    output = fopen(argv[1], "w");
    if (!output)
        return 1;
    fprintf(output, "{\"w\":%d,\"h\":%d,\"calls\":[", w, h);
    az_draw draw;
    screen.on_draw(&draw, (az_rect){0, 0, w, h}, NULL);
    fprintf(output, "]}\n");
    fclose(output);
    return 0;
}
