/* render_settings: draw Anchor's settings page with a stub core to a JSON draw list, so the
 * layout can be rendered and inspected on a computer. Development tool, not part of the mod.
 *   cc -I../anchor -o render_settings render_settings.c ../anchor/settings.c
 *   ./render_settings out.json 1280 420 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

struct az_draw {
    int dummy;
};

typedef struct {
    int kind; /* 0 text, 1 fill, 2 rect */
    int x, y, w, h, px;
    uint32_t col;
    char s[256];
} call_t;

static call_t calls[4096];
static int ncalls;
static int day;
static FILE *out;
static int first = 1;

static int text_width(const char *s, int px)
{
    if (!s) return 0;
    return (int)(0.59976 * px * (double)strlen(s) + 0.5);
}

static void rec(call_t *c)
{
    if (ncalls >= (int)(sizeof calls / sizeof calls[0])) return;
    calls[ncalls++] = *c;
}

static void draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t rgb)
{
    (void)d;
    call_t c;
    memset(&c, 0, sizeof c);
    c.kind = 0;
    c.x = x;
    c.y = y;
    c.px = px;
    c.col = rgb;
    c.w = text_width(s, px);
    c.h = px * 3 / 2;
    snprintf(c.s, sizeof c.s, "%s", s ? s : "");
    rec(&c);
}
static void fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t rgb)
{
    (void)d;
    call_t c;
    memset(&c, 0, sizeof c);
    c.kind = 1;
    c.x = x, c.y = y, c.w = w, c.h = h, c.col = rgb;
    rec(&c);
}
static void draw_rect(az_draw *d, int x, int y, int w, int h, int width, uint32_t rgb)
{
    (void)d;
    for (int i = 0; i < width; i++) {
        call_t c;
        memset(&c, 0, sizeof c);
        c.kind = 2;
        c.x = x + i, c.y = y + i, c.w = w - 2 * i, c.h = h - 2 * i, c.col = rgb;
        if (c.w > 0 && c.h > 0) rec(&c);
    }
}
static uint32_t color(int role)
{
    if (day) {
        switch (role) {
        case AZ_COL_TEXT: return 0x17232b;
        case AZ_COL_TEXT_DIM: return 0x52616e;
        case AZ_COL_HI: return 0x007b91;
        case AZ_COL_HI_TEXT: return 0xffffff;
        case AZ_COL_BG: return 0xf2f5f7;
        default: return 0x17232b;
        }
    }
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
static int cfg_get(const char *id, const char *key, int def)
{
    (void)id;
    (void)key;
    return def;
}
static void cfg_set(const char *id, const char *key, int val)
{
    (void)id;
    (void)key;
    (void)val;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s out.json width height [phrase|cue|grid|ribbon|sidebar|incoming] [day]\n", argv[0]);
        return 2;
    }
    int w = atoi(argv[2]), h = atoi(argv[3]);
    static az_core_v1 core;
    memset(&core, 0, sizeof core);
    core.size = sizeof core;
    core.draw_text = draw_text;
    core.fill_rect = fill_rect;
    core.draw_rect = draw_rect;
    core.color = color;
    core.text_width = text_width;
    core.request_redraw = request_redraw;
    core.cfg_get = cfg_get;
    core.cfg_set = cfg_set;

    day = argc > 5 && !strcmp(argv[5], "day");
    const az_screen_v1 *screen = settings_init(&core, NULL);
    screen->on_enter(NULL);
    if (argc > 4 && !strcmp(argv[4], "cue")) {
        settings_set(SET_RULE, 1); settings_set(SET_HCANY, 0); settings_set(SET_HCSLOT, 7);
    } else if (argc > 4 && !strcmp(argv[4], "grid")) settings_set(SET_RULE, 3);
    else if (argc > 4 && !strcmp(argv[4], "ribbon")) settings_set(SET_DISPLAY, 0);
    else if (argc > 4 && !strcmp(argv[4], "sidebar")) settings_set(SET_DISPLAY, 2);

    az_rect a = { 0, 0, w, h };
    az_draw d;
    screen->on_draw(&d, a, NULL);
    if (w < 900 || h < 420) {
        int steps = argc > 4 && !strcmp(argv[4], "incoming") ? 6 : argc > 4 &&
                    (!strcmp(argv[4], "cue") || !strcmp(argv[4], "grid") || !strcmp(argv[4], "phrase")) ? 2 : 0;
        for (int i = 0; i < steps; i++) {
            az_event ev = {.kind = AZ_EV_KNOB, .dir = 1};
            screen->on_input(&ev, NULL);
            ncalls = 0; screen->on_draw(&d, a, NULL);
        }
    }

    out = fopen(argv[1], "w");
    if (!out) return 1;
    fprintf(out, "{\"w\":%d,\"h\":%d,\"calls\":[", w, h);
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
