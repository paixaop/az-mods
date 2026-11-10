/* SPDX-License-Identifier: GPL-3.0-only */
#include "cues.h"
#include "store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const az_core_v1 *C;
static cues_config config, active_config;
static void (*on_change)(void);
static unsigned revision, active_revision, saved_revision;
static int dirty, draft_pending, storage_ok;
static char storage_status[160], runtime_status[160];
static const az_screen_v1 screen;
enum { GROUPS, GROUP, RULES, RULE, GLOBAL, FILES, DELETE_GROUP, DELETE_RULE };
static int view, group_index, rule_index, page, focus;
static az_rect area;
typedef struct {
    az_rect rect;
    int kind, arg;
} hit;
static hit hits[128];
static int hit_count;
enum { H_TAB, H_FIELD, H_PAGE, H_BACK, H_KEY, H_KEY_PAGE, H_KEY_SAVE, H_KEY_CANCEL };
enum { F_INT, F_DOUBLE, F_TEXT, F_ACTION };
enum {
    A_NEW_GROUP = 1,
    A_OPEN_GROUP,
    A_RULES,
    A_NEW_RULE,
    A_OPEN_RULE,
    A_DELETE_GROUP,
    A_DELETE_RULE,
    A_CONFIRM_GROUP,
    A_CONFIRM_RULE,
    A_IMPORT,
    A_EXPORT,
    A_SAVE,
    A_APPLY,
    A_DISCARD
};
typedef struct {
    const char *label;
    int kind;
    void *value;
    const char *const *choices;
    int count, min, max, action, arg;
    double step, low, high;
    size_t capacity;
} field;
static field fields[32];
static int field_count;
static int keyboard, numeric, key_page, key_return_view, key_return_page;
static char key_label[64], key_buffer[128], key_error[128];
static field key_field;
static const char *const onoff[] = {"Off", "On"};
static const char *const qnames[] = {"Inherit", "Off", "Beat", "Bar"};
static const char *const unit_names[] = {"Bars", "Beats", "Seconds"};
static const char *const anchor_names[] = {"Phrase", "Absolute"};
static const char *const cue_names[] = {"Hot cue", "Memory cue"};
static const char *const points[] = {"Start", "Middle", "End"};
static const char *const loops[] = {"Off", "Fixed length", "From phrase start", "To phrase end"};
static const char *const pads[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
static const char *const indexes[] = {"All", "1st", "2nd", "3rd", "4th", "5th",
                                      "6th", "7th", "8th", "9th", "10th"};
static const char characters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 {}:._-/+()#&'?!,=@";
static const char digits[] = "0123456789.-";
static void redraw(void)
{
    hit_count = 0;
    if (C && AZ_HAS(C, request_redraw))
        C->request_redraw();
}
static void modified(void)
{
    revision++;
    if (!revision)
        revision++;
    draft_pending = 1;
}
static void go(int next)
{
    view = next;
    page = 0;
    focus = 0;
    redraw();
}
static uint32_t color(int role)
{
    return C->color(role);
}
static uint32_t blend(uint32_t a, uint32_t b, int pct)
{
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8)
        out |= ((((a >> s) & 255) * (100 - pct) + ((b >> s) & 255) * pct) / 100) << s;
    return out;
}
static int width(const char *s, int px)
{
    int w = C->text_width(s, px);
    return w > 0 ? w : (int)strlen(s) * px * 3 / 5;
}
static void label(az_draw *d, az_rect r, const char *s, int px, uint32_t col)
{
    if (r.w <= 0 || r.h < px * 3 / 2 || !s)
        return;
    char text[256];
    snprintf(text, sizeof text, "%s", s);
    for (unsigned char *p = (unsigned char *)text; *p; p++)
        if (*p < 32)
            *p = ' ';
    size_t len = strlen(text);
    while (len && width(text, px) > r.w) {
        do {
            len--;
        } while (len && ((unsigned char)text[len] & 0xc0) == 0x80);
        text[len] = 0;
    }
    if (text[0])
        C->draw_text(d, r.x, r.y + (r.h - px * 3 / 2) / 2, text, px, col);
}
static void button(az_draw *d, az_rect r, const char *s, int kind, int arg, int active)
{
    if (r.w < 44 || r.h < 44 || hit_count >= 128)
        return;
    int selected = focus == hit_count;
    C->fill_rect(d, r.x, r.y, r.w, r.h,
                 active ? color(AZ_COL_HI) : blend(color(AZ_COL_BG), color(AZ_COL_TEXT), 7));
    C->draw_rect(d, r.x, r.y, r.w, r.h, 1,
                 active || selected ? color(AZ_COL_HI)
                                    : blend(color(AZ_COL_BG), color(AZ_COL_TEXT), 25));
    label(d, (az_rect){r.x + 10, r.y, r.w - 20, r.h}, s, r.w < 80 ? 12 : 14,
          active ? color(AZ_COL_HI_TEXT) : color(AZ_COL_TEXT));
    hits[hit_count++] = (hit){r, kind, arg};
}
static field *add(const char *name, int kind, void *value)
{
    field *f = &fields[field_count++];
    *f = (field){.label = name, .kind = kind, .value = value};
    return f;
}
static void choice_field(const char *name, int *value, const char *const *choices, int count,
                         int min)
{
    field *f = add(name, F_INT, value);
    f->choices = choices;
    f->count = count;
    f->min = min;
    f->max = min + count - 1;
}
static void integer_field(const char *name, int *value, int low, int high)
{
    field *f = add(name, F_INT, value);
    f->min = low;
    f->max = high;
}
static void number_field(const char *name, double *value, double low, double high, double step)
{
    field *f = add(name, F_DOUBLE, value);
    f->low = low;
    f->high = high;
    f->step = step;
}
static void text_field(const char *name, char *value, size_t capacity)
{
    field *f = add(name, F_TEXT, value);
    f->capacity = capacity;
}
static void action(const char *name, int id, int arg)
{
    field *f = add(name, F_ACTION, NULL);
    f->action = id;
    f->arg = arg;
}
static void rebuild(void)
{
    field_count = 0;
    if (group_index < 0 || group_index >= config.group_count) {
        if (view == GROUP || view == RULES || view == RULE || view == DELETE_GROUP ||
            view == DELETE_RULE)
            view = GROUPS;
        group_index = 0;
    }
    cues_group *g = &config.groups[group_index];
    if (rule_index < 0 || rule_index >= g->rule_count) {
        if (view == RULE || view == DELETE_RULE)
            view = RULES;
        rule_index = 0;
    }
    cues_rule *r = &g->rules[rule_index];
    if (view != FILES && view != DELETE_GROUP && view != DELETE_RULE)
        action(draft_pending ? "Apply draft changes" : "Apply changes", A_APPLY, 0);
    switch (view) {
    case GROUPS:
        if (config.group_count < CUES_MAX_GROUPS)
            action("+ Create group", A_NEW_GROUP, 0);
        for (int i = 0; i < config.group_count; i++)
            action(config.groups[i].name, A_OPEN_GROUP, i);
        break;
    case GROUP:
        action("Edit rules", A_RULES, 0);
        choice_field("Group enabled", &g->enabled, onoff, 2, 0);
        text_field("Group name", g->name, sizeof g->name);
        text_field("Comment", g->comment, sizeof g->comment);
        text_field("Artist contains", g->artist, sizeof g->artist);
        text_field("Musical key", g->key, sizeof g->key);
        number_field("Minimum BPM (0 = global)", &g->min_bpm, 0,
                     g->max_bpm > 0 ? g->max_bpm : config.max_bpm, 1);
        number_field("Maximum BPM (0 = global)", &g->max_bpm, 0, 500, 1);
        choice_field("Quantization", &g->quantization, qnames, 4, 0);
        choice_field("Units for new rules", &g->measures_in, unit_names, 3, 0);
        action("Delete group...", A_DELETE_GROUP, 0);
        break;
    case RULES:
        if (g->rule_count < CUES_MAX_RULES)
            action("+ Create rule", A_NEW_RULE, 0);
        for (int i = 0; i < g->rule_count; i++)
            action(g->rules[i].name[0] ? g->rules[i].name : "Untitled rule", A_OPEN_RULE, i);
        break;
    case RULE:
        choice_field("Rule enabled", &r->enabled, onoff, 2, 0);
        choice_field("Anchor", &r->anchor, anchor_names, 2, 0);
        choice_field("Cue type", &r->cue_type, cue_names, 2, AZ_CUE_HOT);
        if (r->cue_type == AZ_CUE_HOT)
            choice_field("Hot cue pad", &r->pad, pads, 8, 1);
        if (r->anchor == CUES_PHRASE) {
            choice_field("Phrase type", &r->phrase_type, cues_phrase_names, 8, 0);
            choice_field("Occurrence", &r->phrase_index, indexes, 11, 0);
            choice_field("Phrase point", &r->phrase_point, points, 3, 0);
            number_field("Offset (negative = before)", &r->offset, -86400, 86400, 1);
            choice_field("Offset units", &r->offset_unit, unit_names, 3, 0);
        } else {
            number_field("Position (zero-based)", &r->position, 0, 86400, 1);
            choice_field("Position units", &r->unit, unit_names, 3, 0);
        }
        choice_field("Saved loop", &r->loop_mode, loops, r->anchor == CUES_PHRASE ? 4 : 2, 0);
        if (r->loop_mode == CUES_LOOP_FIXED) {
            number_field("Loop length", &r->loop_length, 0.1, 86400, 1);
            choice_field("Loop units", &r->loop_unit, unit_names, 3, 0);
        }
        choice_field("Cue color", &r->color, cues_color_names, 9, 0);
        text_field("Name template", r->name, sizeof r->name);
        text_field("Comment template", r->comment, sizeof r->comment);
        action("Delete rule...", A_DELETE_RULE, 0);
        break;
    case GLOBAL:
        choice_field("Apply on track load", &config.enabled, onoff, 2, 0);
        integer_field("Maximum memory cues", &config.max_memory, 0, 10);
        integer_field("Hot cue pads", &config.hot_pads, 1, 8);
        number_field("Minimum BPM", &config.min_bpm, 1, config.max_bpm, 1);
        number_field("Maximum BPM", &config.max_bpm, config.min_bpm, 500, 1);
        number_field("Minimum cue distance (sec)", &config.min_distance, 0, 3600, 0.1);
        choice_field("Collision preference", &config.cue_preference, cue_names, 2, AZ_CUE_HOT);
        choice_field("Default quantization", &config.quantization, qnames + 1, 3, 1);
        choice_field("Units for new groups", &config.measures_in, unit_names, 3, 0);
        break;
    case FILES:
        action(storage_status[0] ? storage_status : "USB configuration files", 0, 0);
        action("Apply and save changes", A_SAVE, 0);
        action("Discard draft changes", A_DISCARD, 0);
        action("Import cue-rules-import.json", A_IMPORT, 0);
        action("Export cue-rules-export.json", A_EXPORT, 0);
        break;
    case DELETE_GROUP:
        action("Confirm: delete this group", A_CONFIRM_GROUP, 0);
        break;
    case DELETE_RULE:
        action("Confirm: delete this rule", A_CONFIRM_RULE, 0);
        break;
    }
}
static const char *title(void)
{
    switch (view) {
    case GROUP:
        return config.groups[group_index].name;
    case RULES:
        return "Rules";
    case RULE:
        return "Edit cue rule";
    case GLOBAL:
        return "Configuration";
    case FILES:
        return "Import / Export";
    case DELETE_GROUP:
        return "Delete group and all its rules?";
    case DELETE_RULE:
        return "Delete this rule?";
    default:
        return "Rule groups";
    }
}
static int parent_view(void)
{
    return view == GROUP || view == DELETE_GROUP ? GROUPS
           : view == RULES                       ? GROUP
           : view == RULE || view == DELETE_RULE ? RULES
                                                 : GROUPS;
}
static void open_keyboard(field f)
{
    keyboard = 1;
    focus = 0;
    key_field = f;
    numeric = f.kind != F_TEXT;
    key_page = 0;
    key_error[0] = 0;
    key_return_view = view;
    key_return_page = page;
    snprintf(key_label, sizeof key_label, "%s", f.label);
    if (f.kind == F_TEXT)
        snprintf(key_buffer, sizeof key_buffer, "%s", (char *)f.value);
    else if (f.kind == F_INT)
        snprintf(key_buffer, sizeof key_buffer, "%d", *(int *)f.value);
    else
        snprintf(key_buffer, sizeof key_buffer, "%.9g", *(double *)f.value);
    redraw();
}
static void request_file(int operation)
{
    int rc =
        cues_store_request(operation, operation == CUES_STORE_IMPORT ? &config : &active_config,
                           operation == CUES_STORE_IMPORT ? revision : active_revision);
    snprintf(storage_status, sizeof storage_status, "%s",
             rc > 0    ? operation == CUES_STORE_IMPORT ? "Importing USB JSON..."
                                                        : "Exporting USB JSON..."
             : rc == 0 ? "Storage busy; try again shortly"
                       : "SDK storage unavailable");
}
static int apply_changes(void)
{
    char error[128];
    if (!cues_config_validate(&config, error, sizeof error)) {
        snprintf(storage_status, sizeof storage_status, "%s", error);
        return 0;
    }
    if (draft_pending) {
        active_config = config;
        active_revision++;
        if (!active_revision)
            active_revision++;
        draft_pending = 0;
        dirty = 1;
        if (on_change)
            on_change();
    }
    return 1;
}
static void activate_action(int id, int arg)
{
    cues_group *g = &config.groups[group_index];
    switch (id) {
    case A_NEW_GROUP:
        group_index = config.group_count++;
        cues_group_default(&config.groups[group_index], &config);
        snprintf(config.groups[group_index].name, sizeof config.groups[group_index].name,
                 "Group %d", group_index + 1);
        modified();
        go(GROUP);
        break;
    case A_OPEN_GROUP:
        group_index = arg;
        go(GROUP);
        break;
    case A_RULES:
        go(RULES);
        break;
    case A_NEW_RULE:
        rule_index = g->rule_count++;
        cues_rule_default(&g->rules[rule_index], g);
        modified();
        go(RULE);
        break;
    case A_OPEN_RULE:
        rule_index = arg;
        go(RULE);
        break;
    case A_DELETE_GROUP:
        go(DELETE_GROUP);
        break;
    case A_DELETE_RULE:
        go(DELETE_RULE);
        break;
    case A_CONFIRM_GROUP:
        memmove(&config.groups[group_index], &config.groups[group_index + 1],
                (size_t)(config.group_count - group_index - 1) * sizeof(cues_group));
        config.group_count--;
        memset(&config.groups[config.group_count], 0, sizeof(cues_group));
        modified();
        go(GROUPS);
        break;
    case A_CONFIRM_RULE:
        memmove(&g->rules[rule_index], &g->rules[rule_index + 1],
                (size_t)(g->rule_count - rule_index - 1) * sizeof(cues_rule));
        g->rule_count--;
        memset(&g->rules[g->rule_count], 0, sizeof(cues_rule));
        modified();
        go(RULES);
        break;
    case A_IMPORT:
        request_file(CUES_STORE_IMPORT);
        break;
    case A_EXPORT:
        request_file(CUES_STORE_EXPORT);
        break;
    case A_SAVE:
        if (apply_changes())
            request_file(CUES_STORE_SAVE);
        break;
    case A_APPLY:
        apply_changes();
        break;
    case A_DISCARD:
        config = active_config;
        draft_pending = 0;
        revision++;
        go(GROUPS);
        break;
    }
}
static void change_field(int index)
{
    if (index < 0 || index >= field_count)
        return;
    field f = fields[index];
    if (f.kind == F_ACTION) {
        activate_action(f.action, f.arg);
        return;
    }
    if (f.kind == F_TEXT || f.kind == F_DOUBLE || !f.choices) {
        open_keyboard(f);
        return;
    }
    int *value = f.value;
    *value = *value >= f.max ? f.min : *value + 1;
    if (view == RULE && f.value == &config.groups[group_index].rules[rule_index].anchor &&
        *value == CUES_ABSOLUTE) {
        cues_rule *r = &config.groups[group_index].rules[rule_index];
        r->offset = 0;
        if (r->loop_mode >= CUES_LOOP_FROM_START)
            r->loop_mode = CUES_NO_LOOP;
    }
    modified();
    redraw();
}
static int commit_keyboard(void)
{
    cues_config previous = config;
    if (key_field.kind == F_TEXT) {
        if (strlen(key_buffer) >= key_field.capacity)
            return 0;
        snprintf(key_field.value, key_field.capacity, "%s", key_buffer);
    } else {
        char *end;
        double v = strtod(key_buffer, &end);
        if (!key_buffer[0] || *end || !isfinite(v)) {
            snprintf(storage_status, sizeof storage_status, "Enter a valid number");
            return 0;
        }
        if (key_field.kind == F_INT) {
            if (v < key_field.min || v > key_field.max || floor(v) != v) {
                snprintf(storage_status, sizeof storage_status, "Enter an integer from %d to %d",
                         key_field.min, key_field.max);
                return 0;
            }
            *(int *)key_field.value = (int)v;
        } else {
            if (v < key_field.low || v > key_field.high) {
                snprintf(storage_status, sizeof storage_status, "Enter a value from %.9g to %.9g",
                         key_field.low, key_field.high);
                return 0;
            }
            *(double *)key_field.value = v;
        }
    }
    char error[128];
    if (!cues_config_validate(&config, error, sizeof error)) {
        config = previous;
        snprintf(storage_status, sizeof storage_status, "%s", error);
        return 0;
    }
    keyboard = 0;
    view = key_return_view;
    page = key_return_page;
    modified();
    return 1;
}
static void draw_keyboard(az_draw *d, az_rect a)
{
    label(d, (az_rect){a.x + 8, a.y + 3, a.w - 16, 24}, key_error[0] ? key_error : key_label, 14,
          color(AZ_COL_TEXT_DIM));
    label(d, (az_rect){a.x + 8, a.y + 29, a.w - 16, 30}, key_buffer[0] ? key_buffer : "(empty)", 16,
          color(AZ_COL_TEXT));
    int cols = (a.w - 16) / 48;
    if (cols < 1)
        cols = 1;
    int rows = (a.h - 116) / 48;
    if (rows < 1)
        rows = 1;
    int per = rows * cols;
    const char *chars = numeric ? digits : characters;
    int count = (int)strlen(chars) + 2;
    int pages = (count + per - 1) / per;
    if (key_page >= pages)
        key_page = 0;
    int first = key_page * per;
    for (int i = first; i < count && i < first + per; i++) {
        char text[8];
        int arg = i - 2;
        if (i == 0)
            snprintf(text, sizeof text, "Del");
        else if (i == 1)
            snprintf(text, sizeof text, "SP");
        else
            snprintf(text, sizeof text, "%c", chars[arg]);
        int cell = i - first;
        button(d, (az_rect){a.x + 8 + (cell % cols) * 48, a.y + 64 + (cell / cols) * 48, 44, 44},
               text, H_KEY, arg, 0);
    }
    int bw = (a.w - 28) / 4, y = a.y + a.h - 48;
    button(d, (az_rect){a.x + 8, y, bw, 44}, "Cancel", H_KEY_CANCEL, 0, 0);
    button(d, (az_rect){a.x + 12 + bw, y, bw, 44}, "<", H_KEY_PAGE, -1, 0);
    button(d, (az_rect){a.x + 16 + 2 * bw, y, bw, 44}, ">", H_KEY_PAGE, 1, 0);
    button(d, (az_rect){a.x + 20 + 3 * bw, y, bw, 44}, "Save", H_KEY_SAVE, 0, 1);
}
static void draw(az_draw *d, az_rect a, void *unused)
{
    (void)unused;
    area = a;
    hit_count = 0;
    if (!C || a.w < 240 || a.h < 180)
        return;
    C->fill_rect(d, a.x, a.y, a.w, a.h, color(AZ_COL_BG));
    if (keyboard) {
        draw_keyboard(d, a);
        if (focus >= hit_count)
            focus = 0;
        return;
    }
    rebuild();
    int compact = a.w < 640;
    int left = a.x + 12, content = a.w - 24;
    if (content > 1120) {
        left += (content - 1120) / 2;
        content = 1120;
    }
    int tabs_x = compact ? left : left + content - 468, tabs_w = compact ? (content - 8) / 3 : 152;
    if (!compact)
        label(d, (az_rect){left, a.y + 4, content - 484, 44}, "Cues", 26, color(AZ_COL_TEXT));
    int tab = view == GLOBAL ? 1 : view == FILES ? 2 : 0;
    button(d, (az_rect){tabs_x, a.y + 4, tabs_w, 44}, "Groups", H_TAB, 0, tab == 0);
    button(d, (az_rect){tabs_x + tabs_w + 4, a.y + 4, tabs_w, 44}, "Config", H_TAB, 1, tab == 1);
    button(d, (az_rect){tabs_x + 2 * (tabs_w + 4), a.y + 4, tabs_w, 44}, "Files", H_TAB, 2,
           tab == 2);
    int row_top = a.y + 82, row_h = 52, visible = (a.h - 140) / row_h;
    if (visible < 1)
        visible = 1;
    int pages = (field_count + visible - 1) / visible;
    if (pages < 1)
        pages = 1;
    if (page >= pages)
        page = pages - 1;
    label(d, (az_rect){left, a.y + 51, compact ? content - 60 : content, 24}, title(), 16,
          color(AZ_COL_TEXT));
    if (compact) {
        char count[24];
        snprintf(count, sizeof count, "%d/%d", page + 1, pages);
        label(d, (az_rect){left + content - 48, a.y + 51, 48, 24}, count, 12,
              color(AZ_COL_TEXT_DIM));
    }
    for (int i = page * visible; i < field_count && i < (page + 1) * visible; i++) {
        field f = fields[i];
        int y = row_top + (i - page * visible) * row_h;
        if (f.kind == F_ACTION) {
            char value[192];
            snprintf(value, sizeof value, "%s", f.label);
            if (view == GROUPS && f.action == A_OPEN_GROUP)
                snprintf(value, sizeof value, "%s  /  %d rules%s", f.label,
                         config.groups[f.arg].rule_count,
                         config.groups[f.arg].enabled ? "" : "  (off)");
            if (view == RULES && f.action == A_OPEN_RULE) {
                const cues_rule *r = &config.groups[group_index].rules[f.arg];
                snprintf(value, sizeof value, "%s / %s / %s", f.label,
                         r->anchor == CUES_PHRASE ? "Phrase" : "Absolute",
                         r->cue_type == AZ_CUE_HOT ? "Hot cue" : "Memory");
            }
            button(d, (az_rect){left, y, content, 44}, value, H_FIELD, i, 0);
            continue;
        }
        int lw = compact ? content * 44 / 100 : content * 42 / 100;
        label(d, (az_rect){left, y, lw - 10, 44}, f.label, compact ? 12 : 15,
              color(AZ_COL_TEXT_DIM));
        char value[160];
        if (f.kind == F_TEXT)
            snprintf(value, sizeof value, "%s",
                     ((char *)f.value)[0] ? (char *)f.value : "Any / empty");
        else if (f.kind == F_DOUBLE)
            snprintf(value, sizeof value, "%.9g", *(double *)f.value);
        else if (f.choices)
            snprintf(value, sizeof value, "%s", f.choices[*(int *)f.value - f.min]);
        else
            snprintf(value, sizeof value, "%d", *(int *)f.value);
        button(d, (az_rect){left + lw, y, content - lw, 44}, value, H_FIELD, i, 0);
    }
    int fy = a.y + a.h - 48, bw = compact ? 52 : 92;
    button(d, (az_rect){left, fy, bw, 44}, "Back", H_BACK, 0, 0);
    button(d, (az_rect){left + content - 2 * bw - 4, fy, bw, 44}, "<", H_PAGE, -1, 0);
    button(d, (az_rect){left + content - bw, fy, bw, 44}, ">", H_PAGE, 1, 0);
    char summary[320];
    snprintf(summary, sizeof summary, "%d/%d  %s", page + 1, pages,
             draft_pending ? "Draft changes; Apply when ready" : storage_status);
    int statusw = content - 3 * bw - 20;
    if (statusw > 40)
        label(d, (az_rect){left + bw + 10, fy, statusw, 22}, summary, 11, color(AZ_COL_TEXT_DIM));
    if (statusw > 40)
        label(d, (az_rect){left + bw + 10, fy + 22, statusw, 22},
              runtime_status[0]       ? runtime_status
              : active_config.enabled ? "Preserves existing cues"
                                      : "Automatic placement off",
              11, color(AZ_COL_TEXT_DIM));
    if (focus >= hit_count)
        focus = 0;
}
static int input(const az_event *e, void *unused)
{
    (void)unused;
    if (!e || !C)
        return 0;
    if (e->kind == AZ_EV_BACK) {
        if (keyboard) {
            keyboard = 0;
            redraw();
            return 1;
        }
        if (view == GROUPS)
            return 0;
        go(parent_view());
        return 1;
    }
    if (area.w < 240 || area.h < 180 || !hit_count)
        return 0;
    if (e->kind == AZ_EV_KNOB) {
        focus = (focus + (e->dir < 0 ? -1 : 1) + hit_count) % hit_count;
        C->request_redraw();
        return 1;
    }
    hit h;
    if (e->kind == AZ_EV_CLICK) {
        h = hits[focus];
    } else if (e->kind == AZ_EV_TAP) {
        int found = -1;
        for (int i = 0; i < hit_count; i++) {
            az_rect r = hits[i].rect;
            if (e->x >= r.x && e->y >= r.y && e->x < r.x + r.w && e->y < r.y + r.h) {
                found = i;
                break;
            }
        }
        if (found < 0)
            return 0;
        focus = found;
        h = hits[found];
    } else
        return 0;
    switch (h.kind) {
    case H_TAB:
        go(h.arg == 0 ? GROUPS : h.arg == 1 ? GLOBAL : FILES);
        break;
    case H_FIELD:
        change_field(h.arg);
        break;
    case H_PAGE: {
        int visible = (area.h - 140) / 52;
        if (visible < 1)
            visible = 1;
        int pages = (field_count + visible - 1) / visible;
        if (pages < 1)
            pages = 1;
        page = (page + h.arg + pages) % pages;
        break;
    }
    case H_BACK:
        if (view != GROUPS)
            go(parent_view());
        else
            return 0;
        break;
    case H_KEY:
        if (h.arg == -2) {
            size_t n = strlen(key_buffer);
            if (n) {
                do {
                    n--;
                } while (n && ((unsigned char)key_buffer[n] & 0xc0) == 0x80);
                key_buffer[n] = 0;
            }
        } else {
            size_t n = strlen(key_buffer), limit = numeric ? sizeof key_buffer : key_field.capacity;
            if (n + 1 < limit) {
                key_buffer[n] = h.arg == -1 ? ' ' : (numeric ? digits : characters)[h.arg];
                key_buffer[n + 1] = 0;
            }
        }
        break;
    case H_KEY_PAGE: {
        int cols = (area.w - 16) / 48, rows = (area.h - 116) / 48;
        if (rows < 1)
            rows = 1;
        int per = cols * rows, count = (int)strlen(numeric ? digits : characters) + 2,
            pages = (count + per - 1) / per;
        key_page = (key_page + h.arg + pages) % pages;
        break;
    }
    case H_KEY_SAVE:
        if (!commit_keyboard())
            snprintf(key_error, sizeof key_error, "%s", storage_status);
        break;
    case H_KEY_CANCEL:
        keyboard = 0;
        view = key_return_view;
        page = key_return_page;
        break;
    }
    redraw();
    return 1;
}
static void enter(void *unused)
{
    (void)unused;
    keyboard = 0;
    view = GROUPS;
    page = focus = hit_count = 0;
    area = (az_rect){0};
}
const az_screen_v1 *cues_settings_init(const az_core_v1 *core, void (*changed)(void))
{
    C = core;
    on_change = changed;
    revision = active_revision = saved_revision = 1;
    dirty = draft_pending = 0;
    runtime_status[0] = 0;
    storage_status[0] = 0;
    if (!AZ_HAS(core, draw_text) || !AZ_HAS(core, fill_rect) || !AZ_HAS(core, draw_rect) ||
        !AZ_HAS(core, color) || !AZ_HAS(core, text_width) || !AZ_HAS(core, request_redraw))
        return NULL;
    storage_ok = cues_store_init(core, &config, storage_status, sizeof storage_status);
    active_config = config;
    enter(NULL);
    return &screen;
}
const cues_config *cues_settings_config(void)
{
    return &active_config;
}
void cues_settings_status(const char *status)
{
    if (!status)
        status = "";
    if (strcmp(runtime_status, status)) {
        snprintf(runtime_status, sizeof runtime_status, "%s", status);
        if (C && AZ_HAS(C, request_redraw))
            C->request_redraw();
    }
}
void cues_settings_tick(void)
{
    int operation;
    unsigned completed;
    static cues_config imported;
    char status[160];
    int result = cues_store_take(&imported, &operation, &completed, status, sizeof status);
    if (result) {
        snprintf(storage_status, sizeof storage_status, "%s", status);
        if (result > 0 && operation == CUES_STORE_SAVE) {
            saved_revision = completed;
            dirty = active_revision != saved_revision;
        } else if (result > 0 && operation == CUES_STORE_IMPORT) {
            if (completed != revision)
                snprintf(storage_status, sizeof storage_status,
                         "Import discarded: settings changed during read");
            else {
                config = imported;
                keyboard = 0;
                view = GROUPS;
                page = 0;
                modified();
            }
        } else if (result < 0 && operation == CUES_STORE_SAVE)
            dirty = 0;
        redraw();
    }
    if (dirty && storage_ok) {
        int rc = cues_store_request(CUES_STORE_SAVE, &active_config, active_revision);
        if (rc > 0) {
            dirty = 0;
            snprintf(storage_status, sizeof storage_status, "Saving rules to USB...");
        }
    }
}
static const az_screen_v1 screen = {sizeof(az_screen_v1), draw, input, enter, NULL};
