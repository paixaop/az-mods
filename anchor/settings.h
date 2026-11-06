/* SPDX-License-Identifier: GPL-3.0-only
 *
 * settings.h: Anchor's settings values and its page in the Mod Menu.
 *
 * The values live in the core's per-mod settings file (cfg_get/cfg_set), so the page, the
 * guide and a later reinstall all see the same numbers. Quick controls groups guide
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
    SET_DECK,
    SET_COUNT
};

/* Read saved settings and return the settings screen. on_change runs on the UI thread. */
const az_screen_v1 *settings_init(const az_core_v1 *core, void (*on_change)(void));
int settings_get(int key);
void settings_set(int key, int value);

#endif
