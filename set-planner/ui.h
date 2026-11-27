/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SET_PLANNER_UI_H
#define SET_PLANNER_UI_H

#include "mod.h"

enum { SP_HEADER = 54, SP_FOOTER = 48, SP_INSET = 16,
    SP_ROW = 55, SP_LIST_HEADER = 67 };

typedef struct {
    SpRuntime *runtime;
    const az_core_v1 *core;
    az_draw *draw;
    uint32_t bg, stripe, head, rule, select, text, dim, selected_text, action;
    uint32_t energy, target, tempo, good, warning, clash;
} Paint;

enum { SP_RAIL_HEIGHT = 21 };
typedef struct { az_rect chart, detail, list, plot, zoom, left, right, rail; } SpReviewLayout;
typedef struct { az_rect card, plot, preset, reset; } SpSetupLayout;

uint32_t sp_mix(uint32_t from, uint32_t to, unsigned percent);
void sp_rect(const Paint *p, int x, int y, int w, int h, uint32_t color);
void sp_line(const Paint *p, int x0, int y0, int x1, int y1, int width, uint32_t color);
void sp_label(const Paint *p, int x, int y, const char *value, int px, int width, uint32_t color);
void sp_card(const Paint *p, az_rect box);
bool sp_contains(az_rect box, int x, int y);
bool sp_dashboard(const SpRuntime *runtime);
const char *sp_display_title(const SpTrack *track);
void sp_energy_value(const SpRuntime *r, const SpInput *input, char *value, size_t size);
SpSetupLayout sp_setup_layout(az_rect area);
SpReviewLayout sp_review_layout(const SpRuntime *runtime);
void sp_draw_setup_arc(const Paint *p, const char *energy_source);
void sp_draw_set_chart(const Paint *p, SpReviewLayout layout);
void sp_draw_review(const Paint *p, SpReviewLayout layout);
void sp_tap_review(SpRuntime *r, const az_event *event);

#endif
