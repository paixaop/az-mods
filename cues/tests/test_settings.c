/* SPDX-License-Identifier: GPL-3.0-only */
#include "cues.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct az_draw {
    int unused;
};
#include "../settings.c"
static az_rect bounds, texts[512];
static int text_count, events, redraws;
static int pending_save, completed_result;
static unsigned pending_rev;
static cues_config stored;
int cues_store_init(const az_core_v1 *core, cues_config *out, char *status, size_t n)
{
    (void)core;
    cues_config_default(out);
    snprintf(status, n, "Create a group to begin");
    return 1;
}
int cues_store_request(const cues_config *value, unsigned rev)
{
    if (pending_save)
        return 0;
    pending_save = 1;
    pending_rev = rev;
    stored = *value;
    return 1;
}
int cues_store_take(unsigned *rev, char *status, size_t n)
{
    if (!completed_result)
        return 0;
    *rev = pending_rev;
    snprintf(status, n, "Worker completed");
    pending_save = 0;
    int result = completed_result;
    completed_result = 0;
    return result;
}
static int text_width(const char *s, int px)
{
    return (int)(strlen(s) * px * .66 + .5);
}
static void rect_check(int x, int y, int w, int h)
{
    assert(w >= 0 && h >= 0);
    assert(x >= bounds.x && y >= bounds.y);
    assert(x + w <= bounds.x + bounds.w && y + h <= bounds.y + bounds.h);
}
static void fill(az_draw *d, int x, int y, int w, int h, uint32_t rgb)
{
    (void)d;
    (void)rgb;
    rect_check(x, y, w, h);
}
static void rect(az_draw *d, int x, int y, int w, int h, int thick, uint32_t rgb)
{
    (void)thick;
    fill(d, x, y, w, h, rgb);
}
static void draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t rgb)
{
    (void)d;
    (void)rgb;
    az_rect r = { x, y, text_width(s, px), px * 3 / 2 };
    rect_check(r.x, r.y, r.w, r.h);
    for (int i = 0; i < text_count; i++) {
        az_rect b = texts[i];
        assert(!(r.x < b.x + b.w && b.x < r.x + r.w && r.y < b.y + b.h && b.y < r.y + r.h));
    }
    assert(text_count < 512);
    texts[text_count++] = r;
}
static uint32_t palette(int role)
{
    return role == AZ_COL_BG        ? 0x181818
           : role == AZ_COL_HI      ? 0xff8a00
           : role == AZ_COL_HI_TEXT ? 0x101010
                                    : 0xf0f0f0;
}
static void redraw_request(void)
{
    redraws++;
}
static void changed(void)
{
    events++;
}
static az_draw d;
static void paint(int w, int h)
{
    bounds = (az_rect){ 37, 81, w, h };
    text_count = 0;
    screen.on_draw(&d, bounds, NULL);
    for (int i = 0; i < hit_count; i++) {
        az_rect a = hits[i].rect;
        rect_check(a.x, a.y, a.w, a.h);
        assert(a.w >= 44 && a.h >= 44);
        for (int j = 0; j < i; j++) {
            az_rect b = hits[j].rect;
            assert(!(a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h));
        }
    }
}
static void tap_hit(int i)
{
    az_rect r = hits[i].rect;
    az_event e = { .kind = AZ_EV_TAP, .x = r.x + r.w / 2, .y = r.y + r.h / 2 };
    assert(screen.on_input(&e, NULL));
    assert(!hit_count);
}
static void press(int kind, int arg)
{
    for (int i = 0; i < hit_count; i++)
        if (hits[i].kind == kind && hits[i].arg == arg) {
            tap_hit(i);
            return;
        }
    assert(!"missing control");
}
static void press_action(int action_id)
{
    for (int attempts = 0; attempts < 32; attempts++) {
        paint(800, 420);
        for (int i = 0; i < hit_count; i++)
            if (hits[i].kind == H_FIELD && fields[hits[i].arg].kind == F_ACTION &&
                fields[hits[i].arg].action == action_id) {
                tap_hit(i);
                return;
            }
        press(H_PAGE, 1);
    }
    assert(!"missing action");
}
static void press_field(const char *name)
{
    for (int attempts = 0; attempts < 32; attempts++) {
        paint(800, 420);
        for (int i = 0; i < hit_count; i++)
            if (hits[i].kind == H_FIELD && !strcmp(fields[hits[i].arg].label, name)) {
                tap_hit(i);
                return;
            }
        press(H_PAGE, 1);
    }
    assert(!"missing field");
}
static void key(int arg)
{
    for (int attempts = 0; attempts < 50; attempts++) {
        paint(800, 420);
        for (int i = 0; i < hit_count; i++)
            if (hits[i].kind == H_KEY && hits[i].arg == arg) {
                tap_hit(i);
                return;
            }
        press(H_KEY_PAGE, 1);
    }
    assert(!"missing key");
}
static void check_pad_field(int disabled, const char *display)
{
    page = 0;
    paint(1280, 800);
    int index = -1;
    for (int i = 0; i < field_count; ++i)
        if (!strcmp(fields[i].label, "Hot cue pad")) index = i;
    assert(index >= 0 && fields[index].disabled == disabled);
    if (display) assert(fields[index].display && !strcmp(fields[index].display, display));
    int interactive = 0;
    for (int i = 0; i < hit_count; ++i)
        interactive |= hits[i].kind == H_FIELD && hits[i].arg == index;
    assert(interactive == !disabled); /* Touch and browse-knob targets share this list. */
    if (disabled) {
        int pad = config.groups[0].rules[0].pad;
        change_field(index);
        assert(config.groups[0].rules[0].pad == pad && !keyboard);
    }
}
int main(void)
{
    az_core_v1 core = { .size = sizeof core,
                        .abi = AZ_MOD_ABI,
                        .draw_text = draw_text,
                        .fill_rect = fill,
                        .draw_rect = rect,
                        .color = palette,
                        .text_width = text_width,
                        .request_redraw = redraw_request };
    assert(cues_settings_init(&core, changed));
    int status_redraws = redraws;
    cues_settings_status("Waiting for analysis");
    assert(redraws == status_redraws + 1 && !strcmp(runtime_status, "Waiting for analysis"));
    cues_settings_status("Waiting for analysis");
    assert(redraws == status_redraws + 1);
    cues_settings_status(NULL);
    assert(redraws == status_redraws + 2 && !runtime_status[0]);
    screen.on_enter(NULL);
    paint(800, 420);
    press_action(A_NEW_GROUP);
    assert(config.group_count == 1 && view == GROUP);
    assert(cues_settings_config()->group_count == 0 && events == 0);
    assert(draft_pending);
    press_action(A_RULES);
    press_action(A_NEW_RULE);
    assert(config.groups[0].rule_count == 1 && view == RULE);
    assert(cues_settings_config()->group_count == 0 && events == 0);
    check_pad_field(1, "*");
    press_field("Occurrence");
    assert(config.groups[0].rules[0].phrase_index == 1);
    check_pad_field(0, NULL);
    press_field("Hot cue pad");
    assert(config.groups[0].rules[0].pad == 2);
    for (int i = 0; i < 10; ++i) press_field("Occurrence");
    assert(config.groups[0].rules[0].phrase_index == 0);
    check_pad_field(1, "*");
    press_field("Cue type");
    check_pad_field(1, "-");
    press_field("Cue type");
    check_pad_field(1, "*");
    press_field("Anchor");
    assert(config.groups[0].rules[0].anchor == CUES_ABSOLUTE);
    check_pad_field(0, NULL);
    assert(config.groups[0].rules[0].pad == 2);
    press_field("Cue type");
    assert(config.groups[0].rules[0].cue_type == AZ_CUE_MEMORY);
    check_pad_field(1, "-");
    press_field("Position (zero-based)");
    assert(keyboard && numeric);
    snprintf(key_buffer, sizeof key_buffer, "not-a-number");
    paint(800, 420);
    press(H_KEY_SAVE, 0);
    assert(keyboard && key_error[0]);
    snprintf(key_buffer, sizeof key_buffer, "16.5");
    paint(800, 420);
    press(H_KEY_SAVE, 0);
    assert(!keyboard && config.groups[0].rules[0].position == 16.5);
    press_field("Name template");
    assert(keyboard && !numeric);
    while (key_buffer[0])
        key(-2);
    key(0);
    key(1);
    paint(800, 420);
    press(H_KEY_SAVE, 0);
    assert(!strcmp(config.groups[0].rules[0].name, "AB"));
    assert(!cues_settings_config()->group_count);
    press_action(A_APPLY);
    assert(!draft_pending && events == 1 && cues_settings_config()->group_count == 1);
    assert(!strcmp(cues_settings_config()->groups[0].rules[0].name, "AB"));
    cues_settings_tick();
    assert(pending_save && stored.groups[0].rule_count == 1);
    completed_result = 1;
    cues_settings_tick();
    assert(!dirty && !pending_save);
    press_field("Cue type");
    assert(draft_pending && events == 1 &&
           cues_settings_config()->groups[0].rules[0].cue_type == AZ_CUE_MEMORY);
    go(FILES);
    press_action(A_DISCARD);
    assert(!draft_pending && config.groups[0].rules[0].cue_type == AZ_CUE_MEMORY && events == 1);
    go(FILES);
    press_action(A_SAVE);
    assert(pending_save && !dirty);
    assert(strstr(storage_status, "Saving"));
    /* A newer applied draft must still be saved after the pending save finishes. */
    snprintf(config.groups[0].name, sizeof config.groups[0].name, "Newer draft");
    modified();
    press_action(A_SAVE);
    assert(dirty && events == 2);
    assert(strstr(storage_status, "busy"));
    assert(strcmp(stored.groups[0].name, "Newer draft"));
    completed_result = 1;
    cues_settings_tick();
    assert(pending_save && !strcmp(stored.groups[0].name, "Newer draft"));
    completed_result = -1;
    cues_settings_tick();
    assert(!dirty && !pending_save);
    press_action(A_SAVE);
    assert(pending_save && events == 2);
    completed_result = 1;
    cues_settings_tick();
    assert(!dirty && !pending_save);
    /* Deletion is staged and requires its own confirmation before Apply. */
    group_index = 0;
    go(GROUP);
    press_action(A_DELETE_GROUP);
    assert(view == DELETE_GROUP && config.group_count == 1);
    paint(800, 420);
    az_event back = { .kind = AZ_EV_BACK };
    assert(screen.on_input(&back, NULL));
    assert(config.group_count == 1);
    go(DELETE_GROUP);
    press_action(A_CONFIRM_GROUP);
    assert(!config.group_count && cues_settings_config()->group_count == 1);
    press_action(A_APPLY);
    assert(!cues_settings_config()->group_count);
    /* Every page/conditional field/keyboard fits offset and docked rectangles. */
    config.group_count = 1;
    cues_group_default(&config.groups[0], &config);
    config.groups[0].rule_count = 1;
    cues_rule_default(&config.groups[0].rules[0], &config.groups[0]);
    group_index = rule_index = 0;
    assert(screen.flags & AZ_SCREEN_DEDICATED);
    const int sizes[][2] = { { 1216, 535 }, { 1280, 800 }, { 1280, 420 }, { 800, 420 }, { 640, 300 }, { 480, 240 }, { 320, 240 }, { 240, 180 }, { 120, 80 } };
    for (unsigned z = 0; z < sizeof sizes / sizeof sizes[0]; z++)
        for (int v = 0; v <= DELETE_RULE; v++)
            for (int anchor = 0; anchor < 2; anchor++) {
                keyboard = 0;
                view = v;
                config.groups[0].rules[0].anchor = anchor;
                config.groups[0].rules[0].loop_mode = CUES_LOOP_FIXED;
                for (int pg = 0; pg < 8; pg++) {
                    page = pg;
                    paint(sizes[z][0], sizes[z][1]);
                }
                keyboard = 1;
                numeric = 0;
                key_field.capacity = 128;
                for (int pg = 0; pg < 4; pg++) {
                    key_page = pg;
                    paint(sizes[z][0], sizes[z][1]);
                }
            }
    keyboard = 0;
    view = GLOBAL;
    page = 0;
    paint(800, 420);
    az_event knob = { .kind = AZ_EV_KNOB, .dir = 1 };
    int old_focus = focus;
    assert(screen.on_input(&knob, NULL));
    assert(focus != (old_focus));
    paint(800, 420);
    az_event click = { .kind = AZ_EV_CLICK };
    assert(screen.on_input(&click, NULL));
    screen.on_enter(NULL);
    paint(800, 420);
    press(H_TAB, 1);
    assert(view == GLOBAL);
    press_field("Maximum memory cues");
    assert(keyboard && numeric && key_field.kind == F_INT);
    strcpy(key_buffer, "3.5");
    paint(800, 420);
    press(H_KEY_SAVE, 0);
    assert(keyboard && strstr(key_error, "integer"));
    strcpy(key_buffer, "9");
    paint(800, 420);
    press(H_KEY_SAVE, 0);
    assert(!keyboard && config.max_memory == 9);
    paint(800, 420);
    press(H_TAB, 0);
    press_action(A_OPEN_GROUP);
    press_field("Group name");
    paint(240, 180);
    int old_key_page = key_page;
    press(H_KEY_PAGE, 1);
    assert(key_page == old_key_page + 1);
    paint(240, 180);
    press(H_KEY_CANCEL, 0);
    assert(!keyboard && view == GROUP);
    press_action(A_RULES);
    press_action(A_OPEN_RULE);
    assert(view == RULE && config.groups[0].rule_count == 1);
    press_action(A_DELETE_RULE);
    assert(view == DELETE_RULE && config.groups[0].rule_count == 1);
    press_action(A_CONFIRM_RULE);
    assert(view == RULES && !config.groups[0].rule_count);
    paint(800, 420);
    press(H_BACK, 0);
    assert(view == GROUP);
    paint(800, 420);
    press(H_BACK, 0);
    assert(view == GROUPS);
    assert(redraws > 0);
    puts(
        "Settings passed: staged Apply, discard/save, CRUD, text/numeric keyboard, knob, touch "
        "bounds and paging");
    return 0;
}
