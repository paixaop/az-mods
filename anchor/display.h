/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ANCHOR_DISPLAY_H
#define ANCHOR_DISPLAY_H
#include "azmod-mod.h"
#include "guide.h"

enum { ANCHOR_LAYOUT_RIBBON = 1, ANCHOR_LAYOUT_CARD, ANCHOR_LAYOUT_SIDEBAR };
enum { AN_COUNT_NONE, AN_COUNT_BARS, AN_COUNT_BEATS };
#define AN_COLOR_BARS 0x00d6f5u
#define AN_COLOR_BEATS 0xffbd54u
#define AN_COLOR_LOOPING 0xcf9bffu

typedef struct {
    char deck[32];
    char heading[48];
    char number[16];
    char beat[8];
    char unit[24];
    char headline[96];
    char ribbon[112];
    char detail[96];
    int accent;
    int count_unit;
    int looping;
    int loop_prepared;
    char b_target[64];
} an_display_t;

/* Shared drawing/input geometry: A picker and B picker. */
void an_display_controls(az_rect area, int layout, az_rect buttons[2]);
#define AN_SIDEBAR_HANDLE_GAP 10

az_rect an_display_rect(int layout);
az_rect an_sidebar_tab_rect(int expanded);
void an_sidebar_tab_draw(const az_core_v1 *core, az_draw *draw, az_rect area, int expanded);
void an_display_model(const an_plan_t *plan, int outgoing, int incoming, an_display_t *out);
void an_display_draw(const az_core_v1 *core, az_draw *draw, az_rect area, int layout,
                     const an_display_t *model);
#endif
