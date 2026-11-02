/* SPDX-License-Identifier: GPL-3.0-only
 *
 * settings.h: Anchor's settings values and its page in the Mod Menu.
 *
 * The values live in the core's per-mod settings file (cfg_get/cfg_set), so the page, the
 * tick and a later reinstall all see the same numbers. Quick controls groups guide
 * placement, outgoing targets and incoming-deck settings. The page uses its supplied
 * rectangle and theme, with paging for docked menus, 44px touch controls and knob navigation.
 */
#ifndef ANCHOR_SETTINGS_H
#define ANCHOR_SETTINGS_H

#include "azmod-mod.h"

enum {
    SET_SHOW = 0,
    SET_DISPLAY,
    SET_RULE,
    SET_PHRASE,
    SET_POINT,
    SET_CONSOL,
    SET_HCANY,
    SET_HCSLOT,
    SET_LOOP, /* legacy saved key only; automatic detection always applies, no UI control */
    SET_DECK,
    SET_REFRESH,
    SET_COUNT
};

extern const char *const SET_RULE_LABELS[4];
extern const char *const SET_DISPLAY_LABELS[3];
extern const char *const SET_PHRASE_LABELS[8];
extern const char *const SET_POINT_LABELS[2];
extern const char *const SET_HOTCUE_LABELS[8];
extern const char *const SET_DECK_LABELS[3];

/* Called from init(): remember the core and read the values. */
void settings_init(const az_core_v1 *core);
int settings_get(int key);
void settings_set(int key, int value);
/* 1 once after the page's Refresh action was used. */
int settings_take_refresh(void);

/* The page: register with register_screen(), and give it to the SCREEN row. */
extern const az_screen_v1 settings_screen;

#endif
