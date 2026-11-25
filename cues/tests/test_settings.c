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
static int text_count, events, redraws, sentence_drawn;
static unsigned help_topics;
static int help_example_drawn;
static struct { az_rect rect; uint32_t rgb; } fills[512];
static int fill_count;
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
    rect_check(x, y, w, h);
    assert(fill_count < 512);
    fills[fill_count].rect = (az_rect){ x, y, w, h };
    fills[fill_count++].rgb = rgb;
}
static void rect(az_draw *d, int x, int y, int w, int h, int thick, uint32_t rgb)
{
    (void)d;
    (void)thick;
    (void)rgb;
    rect_check(x, y, w, h);
}
static void draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t rgb)
{
    (void)d;
    (void)rgb;
    if (!strncmp(s, "Create a ", 9)) sentence_drawn = 1;
    if (help_open) {
        for (size_t i = 0; i < sizeof help_steps / sizeof help_steps[0]; i++)
            if (!strcmp(s, help_steps[i].heading)) help_topics |= 1u << i;
        if (strstr(s, "-16 bars")) help_example_drawn = 1;
    }
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
    fill_count = 0;
    sentence_drawn = 0;
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
static void choose(const char *name, int value)
{
    press_field(name);
    paint(800, 420);
    assert(rule_choice >= 0);
    press(H_CHOICE, value);
    assert(rule_choice == -1);
}
static void check_field(const char *name, int present)
{
    page = 0;
    paint(1280, 800);
    int index = -1;
    for (int i = 0; i < field_count; ++i)
        if (!strcmp(fields[i].label, name)) index = i;
    int interactive = 0;
    for (int i = 0; i < hit_count; ++i)
        interactive |= hits[i].kind == H_FIELD && hits[i].arg == index;
    assert((index >= 0) == present && interactive == present);
}
static az_rect drawn_rule(int rule)
{
    for (int i = 0; i < hit_count; ++i) {
        if (hits[i].kind != H_FIELD) continue;
        const field *f = &fields[hits[i].arg];
        if (f->action == A_OPEN_RULE && f->arg == rule) return hits[i].rect;
    }
    assert(!"rule card missing from this page");
    return (az_rect){ 0 };
}
static uint32_t card_fill(az_rect card)
{
    for (int i = 0; i < fill_count; ++i)
        if (fills[i].rect.x == card.x && fills[i].rect.y == card.y &&
            fills[i].rect.w == card.w && fills[i].rect.h == card.h)
            return fills[i].rgb;
    assert(!"rule card fill was not drawn");
    return 0;
}
static void check_sentence_padding(az_rect card, int lines)
{
    int top = card.y + card.h, bottom = card.y, count = 0;
    for (int i = 0; i < text_count; ++i) {
        az_rect row = texts[i];
        if (row.x != card.x + 12 || row.y < card.y || row.y >= card.y + card.h) continue;
        if (row.y < top) top = row.y;
        if (row.y + row.h > bottom) bottom = row.y + row.h;
        count++;
    }
    assert(count == lines && card.h == 2 * (bottom - top));
    assert(top - card.y == card.h / 4);
    assert(card.y + card.h - bottom == card.h / 4);
}
static void test_rule_cards(void)
{
    cues_group *g = &config.groups[0];
    g->rule_count = 3;
    for (int i = 0; i < g->rule_count; ++i) {
        cues_rule *r = &g->rules[i];
        cues_rule_default(r, g);
        r->anchor = CUES_ABSOLUTE;
        r->cue_type = AZ_CUE_MEMORY;
        snprintf(r->name, sizeof r->name, "Cue %d", i + 1);
    }
    snprintf(g->rules[1].comment, sizeof g->rules[1].comment,
             "Long review note with a single word cafécafécafécafécafécafécafécafé and a final cue detail");
    go(RULES);
    paint(1216, 535);
    az_rect first = drawn_rule(0), second = drawn_rule(1), third = drawn_rule(2);
    check_sentence_padding(first, 1);
    check_sentence_padding(second, 2);
    check_sentence_padding(third, 1);
    assert(first.h == 48 && second.h == 96 && third.h == 48);
    uint32_t base = card_fill(first), alternate = card_fill(second);
    assert(base != alternate && card_fill(third) == base);
    paint(240, 180);
    assert(page == 0 && pages == 3);
    check_sentence_padding(drawn_rule(0), 1);
    press(H_PAGE, 1);
    paint(240, 180);
    assert(page == 1 && card_fill(drawn_rule(1)) == alternate);
    check_sentence_padding(drawn_rule(1), 1);
    press(H_PAGE, 1);
    paint(240, 180);
    assert(page == 2 && card_fill(drawn_rule(2)) == base);
    press(H_FIELD, field_count - 1);
    assert(view == RULE && rule_index == 2);
}
static az_rect rule_control_hit(int kind, int action_id)
{
    for (int i = 0; i < hit_count; ++i) {
        if (hits[i].kind != kind) continue;
        if (kind != H_FIELD || fields[hits[i].arg].action == action_id) return hits[i].rect;
    }
    assert(!"rule control was not hit-testable");
    return (az_rect){ 0 };
}
static void check_control_icon(az_rect control, int labeled)
{
    int x = control.x + (labeled ? 10 : (control.w - RULE_ICON_SIZE) / 2);
    int y = control.y + (control.h - RULE_ICON_SIZE) / 2;
    for (int i = 0; i < fill_count; ++i) {
        az_rect r = fills[i].rect;
        if (r.h == 1 && r.x >= x && r.x + r.w <= x + RULE_ICON_SIZE &&
            r.y >= y && r.y < y + RULE_ICON_SIZE && fills[i].rgb == palette(AZ_COL_TEXT))
            return;
    }
    assert(!"rule control is missing its theme-colored icon");
}
static void test_rule_toolbar_icons(void)
{
    cues_group *g = &config.groups[0];
    g->rules[0].anchor = CUES_PHRASE;
    open_rule(g, 0);
    paint(1216, 535);
    const int actions[] = { A_RULE_ANCHOR, A_RULE_OFFSET, A_RULE_LOOP, A_DELETE_RULE };
    for (unsigned i = 0; i < sizeof actions / sizeof actions[0]; i++)
        check_control_icon(rule_control_hit(H_FIELD, actions[i]), 1);
    check_control_icon(rule_control_hit(H_SAVE_RULE, 0), 1);
    check_control_icon(rule_control_hit(H_BACK, 0), 1);
    paint(240, 180);
    check_control_icon(rule_control_hit(H_SAVE_RULE, 0), 0);
    check_control_icon(rule_control_hit(H_BACK, 0), 0);
    press(H_BACK, 0);
    assert(view == RULES);
}
static void test_rule_help(void)
{
    const unsigned all_topics = (1u << (sizeof help_steps / sizeof help_steps[0])) - 1;
    go(GROUPS);
    paint(1216, 535);
    press(H_TAB, TAB_HELP);
    assert(help_open && view == GROUPS);
    help_topics = 0;
    help_example_drawn = 0;
    paint(1216, 535);
    assert(pages == 1 && help_topics == all_topics && help_example_drawn);
    press(H_BACK, 0);
    assert(!help_open && view == GROUPS);

    open_rule(&config.groups[0], 0);
    snprintf(rule_edit.name, sizeof rule_edit.name, "Unsaved edit");
    paint(240, 180);
    press(H_PAGE, 1);
    paint(240, 180);
    int editor_page = page;
    assert(editor_page > 0);
    press(H_TAB, TAB_HELP);
    assert(help_open && view == RULE);
    help_topics = 0;
    paint(240, 180);
    assert(pages > 1);
    int help_pages = pages;
    for (int i = 1; i < help_pages; i++) {
        press(H_PAGE, 1);
        paint(240, 180);
        assert(page == i);
    }
    assert(help_topics == all_topics);
    az_event back = { .kind = AZ_EV_BACK };
    assert(screen.on_input(&back, NULL));
    assert(!help_open && view == RULE && page == editor_page);
    assert(!strcmp(rule_edit.name, "Unsaved edit"));
    assert(strcmp(config.groups[0].rules[0].name, "Unsaved edit"));
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
    go(GLOBAL);
    press_field("Consolidate phrases");
    assert(config.consolidate_phrases && !cues_settings_config()->consolidate_phrases);
    go(GROUPS);
    paint(800, 420);
    press_action(A_NEW_GROUP);
    assert(config.group_count == 1 && view == GROUP);
    assert(config.groups[0].quantization == CUES_INHERIT);
    assert(cues_settings_config()->group_count == 0 && events == 0);
    assert(draft_pending);
    press_action(A_RULES);
    press_action(A_NEW_RULE);
    assert(!config.groups[0].rule_count && view == RULE);
    assert(cues_settings_config()->group_count == 0 && events == 0);
    check_field("Hot cue pad", 0);
    paint(800, 420);
    press(H_BACK, 0);
    assert(view == RULES && !config.groups[0].rule_count);
    press_action(A_NEW_RULE);
    choose("Occurrence", 1);
    check_field("Hot cue pad", 1);
    choose("Hot cue pad", 2);
    choose("Occurrence", 0);
    check_field("Hot cue pad", 0);
    choose("Cue color", CUES_COLOR_BY_PAD);
    assert(rule_edit.color == CUES_COLOR_BY_PAD);
    choose("Cue type", AZ_CUE_MEMORY);
    assert(rule_edit.color == CUES_COLOR_RED);
    check_field("Hot cue pad", 0);
    choose("Cue type", AZ_CUE_HOT);
    check_field("Hot cue pad", 0);
    press_action(A_RULE_OFFSET);
    check_field("Offset units", 1);
    choose("Offset units", CUES_BEATS);
    press_field("Offset (negative = before)");
    strcpy(key_buffer, "-16");
    paint(800, 420);
    press(H_KEY_SAVE, 0);
    assert(rule_edit.offset == -16 && rule_edit.offset_unit == CUES_BEATS);
    press_action(A_RULE_OFFSET);
    check_field("Offset units", 0);
    press_action(A_RULE_OFFSET);
    check_field("Offset units", 1);
    assert(rule_edit.offset == -16);
    press_action(A_RULE_LOOP);
    choose("Loop units", CUES_SECONDS);
    assert(rule_edit.loop_mode == CUES_LOOP_FIXED && rule_edit.loop_unit == CUES_SECONDS);
    choose("Loop units", CUES_LOOP_TO_END + 1);
    check_field("Loop length", 0);
    assert(rule_edit.loop_mode == CUES_LOOP_TO_END);
    press_action(A_RULE_LOOP);
    check_field("Loop units", 0);
    press_action(A_RULE_LOOP);
    check_field("Loop units", 1);
    assert(rule_edit.loop_mode == CUES_LOOP_TO_END);
    press_action(A_RULE_ENABLED);
    assert(!rule_edit.enabled);
    press_action(A_RULE_ENABLED);
    press_action(A_RULE_ANCHOR);
    assert(rule_edit.anchor == CUES_ABSOLUTE && !rule_loop);
    check_field("Hot cue pad", 1);
    assert(rule_edit.pad == 2);
    check_field("Offset units", 0);
    choose("Cue type", AZ_CUE_MEMORY);
    assert(rule_edit.cue_type == AZ_CUE_MEMORY);
    check_field("Hot cue pad", 0);
    press_field("Position units");
    paint(240, 180);
    press(H_PAGE, 1);
    paint(240, 180);
    press(H_PAGE, 1);
    paint(240, 180);
    press(H_CHOICE, CUES_SECONDS);
    assert(rule_edit.unit == CUES_SECONDS);
    press_field("Position (zero-based)");
    assert(keyboard && numeric);
    snprintf(key_buffer, sizeof key_buffer, "not-a-number");
    paint(800, 420);
    press(H_KEY_SAVE, 0);
    assert(keyboard && key_error[0]);
    snprintf(key_buffer, sizeof key_buffer, "16.5");
    paint(800, 420);
    press(H_KEY_SAVE, 0);
    assert(!keyboard && rule_edit.position == 16.5);
    press_field("Name template");
    assert(keyboard && !numeric);
    while (key_buffer[0])
        key(-2);
    key(0);
    key(1);
    paint(800, 420);
    press(H_KEY_SAVE, 0);
    assert(!config.groups[0].rule_count && !strcmp(rule_edit.name, "AB"));
    paint(800, 420);
    press(H_SAVE_RULE, 0);
    paint(800, 420);
    assert(view == RULES && sentence_drawn && config.groups[0].rule_count == 1);
    assert(config.groups[0].rules[0].offset == 0);
    assert(!strcmp(config.groups[0].rules[0].name, "AB"));
    assert(!cues_settings_config()->group_count);
    press_action(A_APPLY);
    assert(!draft_pending && events == 1 && cues_settings_config()->group_count == 1);
    assert(cues_settings_config()->consolidate_phrases);
    assert(!strcmp(cues_settings_config()->groups[0].rules[0].name, "AB"));
    cues_settings_tick();
    assert(pending_save && stored.groups[0].rule_count == 1);
    char json[4096], error[128];
    cues_config restored;
    int length = cues_config_json(&stored, json, sizeof json);
    assert(length > 0 && strstr(json, "\"sentence\":\"Create a Memory Cue, at 16.5 seconds"));
    assert(cues_config_parse(&restored, json, (size_t)length, error, sizeof error));
    assert(restored.groups[0].rules[0].position == 16.5);
    assert(!strcmp(restored.groups[0].rules[0].name, "AB"));
    completed_result = 1;
    cues_settings_tick();
    assert(!dirty && !pending_save);
    press_action(A_OPEN_RULE);
    press_field("Cue type");
    paint(800, 420);
    press(H_BACK, 0);
    assert(view == RULE && rule_edit.cue_type == AZ_CUE_MEMORY);
    choose("Cue type", AZ_CUE_HOT);
    paint(800, 420);
    press(H_BACK, 0);
    assert(!draft_pending && config.groups[0].rules[0].cue_type == AZ_CUE_MEMORY);
    press_action(A_OPEN_RULE);
    choose("Cue type", AZ_CUE_HOT);
    paint(800, 420);
    press(H_SAVE_RULE, 0);
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
    completed_result = -1; /* Failure of the older save must not drop this draft. */
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
                go(v);
                config.groups[0].rules[0].anchor = anchor;
                config.groups[0].rules[0].loop_mode = CUES_LOOP_FIXED;
                if (view == RULE) open_rule(&config.groups[0], 0);
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
    assert(view == RULES);
    press_action(A_EDIT_GROUP);
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
    assert(screen.on_input(&back, NULL) && view == RULE);
    press_action(A_DELETE_RULE);
    press_action(A_CONFIRM_RULE);
    assert(view == RULES && !config.groups[0].rule_count);
    paint(800, 420);
    press(H_BACK, 0);
    assert(view == GROUPS);
    assert(redraws > 0);
    test_rule_cards();
    test_rule_toolbar_icons();
    test_rule_help();
    puts(
        "Settings passed: staged Apply, discard/save, CRUD, text/numeric keyboard, knob, touch "
        "bounds and paging");
    return 0;
}
