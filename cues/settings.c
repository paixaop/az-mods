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
static unsigned active_revision, saved_revision, saving_revision, failed_revision;
static int dirty, draft_pending, storage_ok;
static char storage_status[160], runtime_status[160];
static const az_screen_v1 screen;
enum { GROUPS,
       GROUP,
       RULES,
       RULE,
       GLOBAL,
       FILES,
       DELETE_GROUP,
       DELETE_RULE };
static int view, group_index, rule_index, page, pages, focus;
static int help_open, help_return_page;
static az_rect area;
static cues_rule rule_edit;
static int rule_offset, rule_loop, rule_loop_option, rule_choice = -1, choice_page, choice_pages;
typedef struct {
    az_rect rect;
    int kind, arg;
} hit;
static hit hits[128];
static int hit_count;
enum { H_TAB,
       H_FIELD,
       H_CHOICE,
       H_SAVE_RULE,
       H_PAGE,
       H_BACK };
enum { TAB_GROUPS, TAB_CONFIG, TAB_FILES, TAB_HELP, TAB_COUNT };
static const struct { const char *label; int view; } tabs[TAB_COUNT] = {
    [TAB_GROUPS] = { "Groups", GROUPS }, [TAB_CONFIG] = { "Config", GLOBAL },
    [TAB_FILES] = { "Files", FILES }, [TAB_HELP] = { "Help", -1 },
};
enum { F_INT,
       F_DOUBLE,
       F_TEXT,
       F_ACTION };
enum {
    A_NEW_GROUP = 1,
    A_OPEN_GROUP,
    A_EDIT_GROUP,
    A_RULES,
    A_NEW_RULE,
    A_OPEN_RULE,
    A_DELETE_GROUP,
    A_DELETE_RULE,
    A_CONFIRM_GROUP,
    A_CONFIRM_RULE,
    A_RULE_ANCHOR,
    A_RULE_OFFSET,
    A_RULE_LOOP,
    A_RULE_ENABLED,
    A_SAVE,
    A_APPLY,
    A_DISCARD
};
enum rule_icon {
    RULE_ICON_NONE, RULE_ICON_ANCHOR, RULE_ICON_OFFSET, RULE_ICON_LOOP,
    RULE_ICON_DELETE, RULE_ICON_SAVE, RULE_ICON_CANCEL
};
enum { RULE_ICON_SIZE = 14, RULE_ICON_GAP = 6 };
/* Toolbar glyphs sized for the deck's native pixels. Each row is read left to right. */
static const char rule_icons[][RULE_ICON_SIZE][RULE_ICON_SIZE + 1] = {
    [RULE_ICON_ANCHOR] = {
        ".....####.....", ".....#..#.....", ".....#..#.....", ".....####.....",
        "......##......", "....######....", "......##......", "#.....##.....#",
        "##....##....##", ".#....##....#.", ".##...##...##.", "..##..##..##..",
        "...########...", ".....####.....",
    },
    [RULE_ICON_OFFSET] = {
        "..............", "..............", "...##....##...", "..##......##..",
        ".##........##.", "##..........##", "##############", "##############",
        "##..........##", ".##........##.", "..##......##..", "...##....##...",
        "..............", "..............",
    },
    [RULE_ICON_LOOP] = {
        "........##....", ".........##...", "..##########..", ".############.",
        "##.......##...", "##......##....", "##..........##", "##..........##",
        "....##......##", "...##.......##", ".############.", "..##########..",
        "...##.........", "....##........",
    },
    [RULE_ICON_DELETE] = {
        ".....####.....", "...########...", "..##########..", "..............",
        "...########...", "...##....##...", "...##.##.##...", "...##.##.##...",
        "...##.##.##...", "...##.##.##...", "...##.##.##...", "...##....##...",
        "...########...", "....######....",
    },
    [RULE_ICON_SAVE] = {
        "..............", "............#.", "...........###", "..........###.",
        ".........###..", "........###...", ".......###....", "##....###.....",
        "###..###......", ".######.......", "..####........", "...##.........",
        "..............", "..............",
    },
    [RULE_ICON_CANCEL] = {
        "..............", ".##........##.", ".###......###.", "..###....###..",
        "...###..###...", "....######....", ".....####.....", ".....####.....",
        "....######....", "...###..###...", "..###....###..", ".###......###.",
        ".##........##.", "..............",
    },
};
typedef struct {
    const char *label;
    const char *display;
    int kind, active;
    enum rule_icon icon;
    void *value;
    const char *const *choices;
    int count, min, max, action, arg;
    double step, low, high;
    size_t capacity;
} field;
static field fields[32];
static int field_count;
typedef struct {
    const char *text;
    int field_index, line_start;
} sentence_token;
static sentence_token sentence[32];
static int sentence_count;
static int editing_field;
static field entry_field;
static const char *const onoff[] = { "Off", "On" };
static const char *const qnames[] = { "Inherit", "Off", "Beat", "Bar" };
static const char *const unit_names[] = { "Bars", "Beats", "Seconds" };
static const char *const anchor_names[] = { "Phrase", "Absolute" };
static const char *const cue_names[] = { "Hot cue", "Memory cue" };
static const char *const points[] = { "start", "middle", "end" };
static const char *const loop_options[] = { "bars", "beats", "seconds", "from phrase start", "to phrase end" };
static const char *const pads[] = { "A", "B", "C", "D", "E", "F", "G", "H" };
static void redraw(void)
{
    hit_count = 0;
    if (C && AZ_HAS(C, request_redraw))
        C->request_redraw();
}
static void modified(void)
{
    draft_pending = 1;
}
static void go(int next)
{
    view = next;
    help_open = 0;
    rule_choice = -1;
    page = 0;
    focus = 0;
    redraw();
}
static void open_help(void)
{
    help_return_page = page;
    help_open = 1;
    page = focus = 0;
    redraw();
}
static void close_help(void)
{
    help_open = 0;
    page = help_return_page;
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
static void fit_label(char *text, int px, int available)
{
    for (unsigned char *p = (unsigned char *)text; *p; p++)
        if (*p < 32)
            *p = ' ';
    size_t len = strlen(text);
    while (len && width(text, px) > available) {
        do {
            len--;
        } while (len && ((unsigned char)text[len] & 0xc0) == 0x80);
        text[len] = 0;
    }
}

static void label(az_draw *d, az_rect r, const char *s, int px, uint32_t col)
{
    if (r.w <= 0 || r.h < px * 3 / 2 || !s)
        return;
    char text[256];
    snprintf(text, sizeof text, "%s", s);
    fit_label(text, px, r.w);
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
    label(d, (az_rect){ r.x + 10, r.y, r.w - 20, r.h }, s, r.w < 80 ? 12 : 14,
          active ? color(AZ_COL_HI_TEXT) : color(AZ_COL_TEXT));
    hits[hit_count++] = (hit){ r, kind, arg };
}
static field *add(const char *name, int kind, void *value)
{
    field *f = &fields[field_count++];
    *f = (field){ .label = name, .kind = kind, .value = value };
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
static void repair_group_selection(void)
{
    if (group_index < 0 || group_index >= config.group_count) {
        if (view == GROUP || view == RULES || view == RULE || view == DELETE_GROUP ||
            view == DELETE_RULE)
            view = GROUPS;
        group_index = 0;
    }
}
static void build_groups(void)
{
    if (config.group_count < CUES_MAX_GROUPS)
        action("+ Add group", A_NEW_GROUP, 0);
    for (int i = 0; i < config.group_count; i++)
        action(config.groups[i].name, A_OPEN_GROUP, i);
}
static void build_group(cues_group *g)
{
    action("View rules", A_RULES, 0);
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
}
static void build_rules(cues_group *g)
{
    action("Edit group", A_EDIT_GROUP, 0);
    if (g->rule_count < CUES_MAX_RULES)
        action("+ Add rule", A_NEW_RULE, 0);
    for (int i = 0; i < g->rule_count; i++)
        action("Edit rule", A_OPEN_RULE, i);
}
static int new_rule(void)
{
    return rule_index == config.groups[group_index].rule_count;
}
static void sentence_text(const char *text, int line_start)
{
    sentence[sentence_count++] = (sentence_token){ text, -1, line_start };
}
static void sentence_value(void)
{
    sentence[sentence_count++] = (sentence_token){ NULL, field_count - 1, 0 };
}
static void rule_control(const char *name, int id, int active, enum rule_icon icon)
{
    action(name, id, 0);
    fields[field_count - 1].active = active;
    fields[field_count - 1].icon = icon;
    sentence_value();
}
static void build_rule_toolbar(const cues_rule *r)
{
    rule_control(anchor_names[r->anchor], A_RULE_ANCHOR, 0, RULE_ICON_ANCHOR);
    if (r->anchor == CUES_PHRASE)
        rule_control("Offset", A_RULE_OFFSET, rule_offset, RULE_ICON_OFFSET);
    rule_control("Loop", A_RULE_LOOP, rule_loop, RULE_ICON_LOOP);
    rule_control(r->enabled ? "Enabled" : "Disabled", A_RULE_ENABLED, 0, RULE_ICON_NONE);
    if (!new_rule())
        rule_control("Delete...", A_DELETE_RULE, 0, RULE_ICON_DELETE);
}
static void build_cue_clause(cues_rule *r)
{
    sentence_text("Create a", 1);
    choice_field("Cue type", &r->cue_type, cue_names, 2, AZ_CUE_HOT);
    sentence_value();
    if (r->cue_type == AZ_CUE_MEMORY)
        return;
    if (cues_rule_auto_pad(r)) {
        sentence_text("on the next free pad", 0);
    } else {
        sentence_text("on pad", 0);
        choice_field("Hot cue pad", &r->pad, pads, 8, 1);
        sentence_value();
    }
}
static void build_phrase_clause(cues_rule *r)
{
    sentence_text("at the", 1);
    choice_field("Phrase point", &r->phrase_point, points, 3, 0);
    sentence_value();
    sentence_text(r->phrase_index ? "of the" : "of", 0);
    choice_field("Occurrence", &r->phrase_index, cues_occurrences, 11, 0);
    sentence_value();
    choice_field("Phrase type", &r->phrase_type, cues_phrase_names, 8, 0);
    sentence_value();
    sentence_text(r->phrase_index ? "phrase" : "phrases", 0);
    if (rule_offset) {
        sentence_text("offset by", 0);
        number_field("Offset (negative = before)", &r->offset, -86400, 86400, 1);
        sentence_value();
        choice_field("Offset units", &r->offset_unit, loop_options, 3, 0);
        sentence_value();
    }
}
static void build_absolute_clause(cues_rule *r)
{
    sentence_text("at", 1);
    number_field("Position (zero-based)", &r->position, 0, 86400, 1);
    sentence_value();
    choice_field("Position units", &r->unit, loop_options, 3, 0);
    sentence_value();
}
static int loop_option(const cues_rule *r)
{
    return r->loop_mode == CUES_LOOP_FIXED ? r->loop_unit : r->loop_mode + 1;
}
static void build_loop_clause(cues_rule *r)
{
    if (!rule_loop)
        return;
    sentence_text("and loop", 0);
    if (r->loop_mode == CUES_LOOP_FIXED) {
        number_field("Loop length", &r->loop_length, 0.1, 86400, 1);
        sentence_value();
    }
    rule_loop_option = loop_option(r);
    choice_field("Loop units", &rule_loop_option, loop_options, r->anchor == CUES_PHRASE ? 5 : 3, 0);
    sentence_value();
}
static void build_style_clause(cues_rule *r)
{
    sentence_text("set color to", 1);
    choice_field("Cue color", &r->color, cues_color_names,
                 r->cue_type == AZ_CUE_HOT ? CUES_COLOR_COUNT : CUES_COLOR_BY_PAD, 0);
    sentence_value();
    sentence_text("and name it", 1);
    text_field("Name template", r->name, sizeof r->name);
    fields[field_count - 1].display = r->name[0] ? NULL : "(empty)";
    sentence_value();
    sentence_text("with comment", 0);
    text_field("Comment template", r->comment, sizeof r->comment);
    fields[field_count - 1].display = r->comment[0] ? NULL : "(optional)";
    sentence_value();
}
static void build_rule(void)
{
    sentence_count = 0;
    build_rule_toolbar(&rule_edit);
    build_cue_clause(&rule_edit);
    if (rule_edit.anchor == CUES_PHRASE)
        build_phrase_clause(&rule_edit);
    else
        build_absolute_clause(&rule_edit);
    build_loop_clause(&rule_edit);
    build_style_clause(&rule_edit);
}
static void build_global(void)
{
    choice_field("Apply on track load", &config.enabled, onoff, 2, 0);
    choice_field("Consolidate phrases", &config.consolidate_phrases, onoff, 2, 0);
    integer_field("Maximum memory cues", &config.max_memory, 0, 10);
    integer_field("Hot cue pads", &config.hot_pads, 1, 8);
    number_field("Minimum BPM", &config.min_bpm, 1, config.max_bpm, 1);
    number_field("Maximum BPM", &config.max_bpm, config.min_bpm, 500, 1);
    number_field("Minimum cue distance (sec)", &config.min_distance, 0, 3600, 0.1);
    choice_field("Collision preference", &config.cue_preference, cue_names, 2, AZ_CUE_HOT);
    choice_field("Default quantization", &config.quantization, qnames + 1, 3, 1);
    choice_field("Units for new groups", &config.measures_in, unit_names, 3, 0);
}
static void build_files(void)
{
    action(storage_status[0] ? storage_status : "USB configuration", 0, 0);
    action("Apply and save changes", A_SAVE, 0);
    action("Discard draft changes", A_DISCARD, 0);
}
static void build_view(cues_group *g)
{
    switch (view) {
    case GROUPS: build_groups(); break;
    case GROUP: build_group(g); break;
    case RULES: build_rules(g); break;
    case GLOBAL: build_global(); break;
    case FILES: build_files(); break;
    case DELETE_GROUP:
        action("Confirm: delete this group", A_CONFIRM_GROUP, 0);
        break;
    case DELETE_RULE:
        action("Confirm: delete this rule", A_CONFIRM_RULE, 0);
        break;
    }
}

static void rebuild(void)
{
    field_count = 0;
    if (help_open) return;
    repair_group_selection();
    cues_group *g = &config.groups[group_index];
    if (view == RULE) {
        build_rule();
        return;
    }
    if (view != FILES && view != DELETE_GROUP && view != DELETE_RULE)
        action(draft_pending ? "Apply draft changes" : "Apply changes", A_APPLY, 0);
    build_view(g);
}
static const char *title(void)
{
    switch (view) {
    case GROUP:
    case RULES:
        return config.groups[group_index].name;
    case RULE:
        return new_rule() ? "New rule" : "Edit rule";
    case GLOBAL:
        return "Configuration";
    case FILES:
        return "USB configuration";
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
    if (view == DELETE_RULE) return RULE;
    if (view == RULE || view == GROUP) return RULES;
    if (view == DELETE_GROUP) return GROUP;
    return GROUPS;
}
static void request_save(void)
{
    int rc = cues_store_request(&active_config, active_revision);
    if (rc > 0) {
        saving_revision = active_revision;
        dirty = 0;
    }
    snprintf(storage_status, sizeof storage_status, "%s",
             rc > 0    ? "Saving rules to USB..."
             : rc == 0 ? "Storage busy; try again shortly"
                       : "SDK storage unavailable");
}
static void commit_config(const cues_config *candidate)
{
    active_config = *candidate;
    if (++active_revision == 0)
        ++active_revision;
    draft_pending = 0;
    dirty = 1;
    if (on_change)
        on_change();
}

static int apply_changes(void)
{
    char error[128];
    if (!cues_config_validate(&config, error, sizeof error)) {
        snprintf(storage_status, sizeof storage_status, "%s", error);
        return 0;
    }
    if (draft_pending)
        commit_config(&config);
    return 1;
}
static int group_action(int id, int arg)
{
    switch (id) {
    case A_NEW_GROUP:
        group_index = config.group_count++;
        cues_group_default(&config.groups[group_index], &config);
        snprintf(config.groups[group_index].name, sizeof config.groups[group_index].name,
                 "Group %d", group_index + 1);
        modified();
        go(GROUP);
        return 1;
    case A_OPEN_GROUP:
        group_index = arg;
        go(RULES);
        return 1;
    case A_EDIT_GROUP:
        go(GROUP);
        return 1;
    case A_DELETE_GROUP:
        go(DELETE_GROUP);
        return 1;
    case A_CONFIRM_GROUP:
        memmove(&config.groups[group_index], &config.groups[group_index + 1],
                (size_t)(config.group_count - group_index - 1) * sizeof(cues_group));
        config.group_count--;
        memset(&config.groups[config.group_count], 0, sizeof(cues_group));
        modified();
        go(GROUPS);
        return 1;
    default: return 0;
    }
}
static void open_rule(const cues_group *g, int index)
{
    rule_index = index;
    if (new_rule())
        cues_rule_default(&rule_edit, g);
    else
        rule_edit = g->rules[index];
    rule_offset = rule_edit.offset != 0;
    rule_loop = rule_edit.loop_mode != CUES_NO_LOOP;
    if (!rule_loop)
        rule_edit.loop_mode = CUES_LOOP_FIXED;
    go(RULE);
}
static void save_rule(void)
{
    cues_group *g = &config.groups[group_index];
    if (!rule_offset || rule_edit.anchor == CUES_ABSOLUTE)
        rule_edit.offset = 0;
    if (!rule_loop)
        rule_edit.loop_mode = CUES_NO_LOOP;
    if (new_rule())
        g->rule_count++;
    g->rules[rule_index] = rule_edit;
    modified();
    go(RULES);
}
static int rule_action(int id, int arg)
{
    cues_group *g = &config.groups[group_index];
    switch (id) {
    case A_RULES:
        go(RULES);
        return 1;
    case A_NEW_RULE:
        open_rule(g, g->rule_count);
        return 1;
    case A_OPEN_RULE:
        open_rule(g, arg);
        return 1;
    case A_DELETE_RULE:
        go(DELETE_RULE);
        return 1;
    case A_CONFIRM_RULE:
        memmove(&g->rules[rule_index], &g->rules[rule_index + 1],
                (size_t)(g->rule_count - rule_index - 1) * sizeof(cues_rule));
        g->rule_count--;
        memset(&g->rules[g->rule_count], 0, sizeof(cues_rule));
        modified();
        go(RULES);
        return 1;
    default: return 0;
    }
}
static void toggle_rule_anchor(void)
{
    rule_edit.anchor = rule_edit.anchor == CUES_PHRASE ? CUES_ABSOLUTE : CUES_PHRASE;
    if (rule_edit.anchor == CUES_ABSOLUTE && rule_edit.loop_mode >= CUES_LOOP_FROM_START) {
        rule_edit.loop_mode = CUES_LOOP_FIXED;
        rule_loop = 0;
    }
}
static int rule_toolbar_action(int id)
{
    switch (id) {
    case A_RULE_ANCHOR: toggle_rule_anchor(); return 1;
    case A_RULE_OFFSET: rule_offset = !rule_offset; return 1;
    case A_RULE_LOOP: rule_loop = !rule_loop; return 1;
    case A_RULE_ENABLED: rule_edit.enabled = !rule_edit.enabled; return 1;
    default: return 0;
    }
}
static int publish_action(int id)
{
    switch (id) {
    case A_SAVE:
        if (apply_changes())
            request_save();
        return 1;
    case A_APPLY:
        apply_changes();
        return 1;
    case A_DISCARD:
        config = active_config;
        draft_pending = 0;
        go(GROUPS);
        return 1;
    default: return 0;
    }
}

static void activate_action(int id, int arg)
{
    if (group_action(id, arg)) return;
    if (rule_action(id, arg)) return;
    if (rule_toolbar_action(id)) return;
    publish_action(id);
}
static void cycle_choice(const field *f)
{
    int *value = f->value;
    *value = *value >= f->max ? f->min : *value + 1;
}
static void set_rule_loop(int option)
{
    if (option <= CUES_SECONDS) {
        rule_edit.loop_unit = option;
        rule_edit.loop_mode = CUES_LOOP_FIXED;
    } else
        rule_edit.loop_mode = option - 1;
}
static void select_rule_choice(int value)
{
    field *f = &fields[rule_choice];
    if (f->value == &rule_loop_option)
        set_rule_loop(value);
    else
        *(int *)f->value = value;
    if (!cues_rule_color_valid(&rule_edit))
        rule_edit.color = CUES_COLOR_RED;
    rule_choice = -1;
    focus = 0;
}
static void open_field_entry(field f);
static void change_field(int index)
{
    if (index < 0 || index >= field_count)
        return;
    field f = fields[index];
    if (f.kind == F_ACTION) {
        activate_action(f.action, f.arg);
        return;
    }
    if (!f.choices) {
        open_field_entry(f);
        return;
    }
    if (view == RULE) {
        rule_choice = index;
        choice_page = focus = 0;
        return;
    }
    cycle_choice(&f);
    modified();
    redraw();
}
static int commit_integer(double value)
{
    if (value < entry_field.min || value > entry_field.max || floor(value) != value) {
        snprintf(storage_status, sizeof storage_status, "Enter an integer from %d to %d",
                 entry_field.min, entry_field.max);
        return 0;
    }
    *(int *)entry_field.value = (int)value;
    return 1;
}

static int commit_decimal(double value)
{
    if (value < entry_field.low || value > entry_field.high) {
        snprintf(storage_status, sizeof storage_status, "Enter a number from %.9g to %.9g",
                 entry_field.low, entry_field.high);
        return 0;
    }
    *(double *)entry_field.value = value;
    return 1;
}

static int commit_number(const char *text)
{
    char *end;
    double value = strtod(text, &end);
    if (end == text || *end || !isfinite(value)) {
        snprintf(storage_status, sizeof storage_status, "Enter a valid number");
        return 0;
    }
    return entry_field.kind == F_INT ? commit_integer(value) : commit_decimal(value);
}

static int commit_entry(const char *text)
{
    if (entry_field.kind != F_TEXT)
        return commit_number(text);
    if (strlen(text) >= entry_field.capacity)
        return 0;
    snprintf(entry_field.value, entry_field.capacity, "%s", text);
    return 1;
}

static int commit_config_entry(const char *text)
{
    cues_config previous = config;
    if (!commit_entry(text))
        return 0;
    if (!cues_config_validate(&config, storage_status, sizeof storage_status)) {
        config = previous;
        return 0;
    }
    modified();
    return 1;
}

static const char *field_entry_result(int status, const char *text, void *user)
{
    (void)user;
    if (!editing_field)
        return NULL;
    if (status == AZ_OK) {
        storage_status[0] = 0;
        int accepted = view == RULE ? commit_entry(text) : commit_config_entry(text);
        if (!accepted)
            return storage_status[0] ? storage_status : "Value is too long";
    }
    if (status == AZ_FAILED)
        snprintf(storage_status, sizeof storage_status, "Could not open the deck keyboard");
    editing_field = 0;
    redraw();
    return NULL;
}

static void initial_number(char *text, size_t capacity, const field *f)
{
    if (f->kind == F_INT)
        snprintf(text, capacity, "%d", *(int *)f->value);
    else
        snprintf(text, capacity, "%.9g", *(double *)f->value);
}

static void open_field_entry(field f)
{
    if (!AZ_HAS(C, text_entry)) {
        snprintf(storage_status, sizeof storage_status, "Deck keyboard is unavailable");
        return;
    }
    char number[64];
    if (f.kind != F_TEXT)
        initial_number(number, sizeof number, &f);
    const az_text_entry_v1 entry = {
        .size = sizeof entry,
        .max_bytes = f.kind == F_TEXT ? (uint32_t)(f.capacity - 1) : sizeof number - 1,
        .title = f.label,
        .initial = f.kind == F_TEXT ? f.value : number,
        .submit = "Save",
        .on_result = field_entry_result,
    };
    int status = C->text_entry(CUES_ID, &entry);
    if (status != AZ_OK) {
        snprintf(storage_status, sizeof storage_status, "%s", status == AZ_BUSY
                 ? "Another dialog is open" : "Deck keyboard is unavailable");
        return;
    }
    entry_field = f;
    editing_field = 1;
}
static void field_value(const field *f, char *value, size_t capacity)
{
    if (f->display)
        snprintf(value, capacity, "%s", f->display);
    else if (f->kind == F_TEXT)
        snprintf(value, capacity, "%s",
                 ((char *)f->value)[0] ? (char *)f->value : "Any / empty");
    else if (f->kind == F_DOUBLE)
        snprintf(value, capacity, "%.9g", *(double *)f->value);
    else if (f->choices)
        snprintf(value, capacity, "%s", f->choices[*(int *)f->value - f->min]);
    else
        snprintf(value, capacity, "%d", *(int *)f->value);
}

static void token_caption(const sentence_token *token, char *caption, size_t capacity)
{
    if (token->text) {
        snprintf(caption, capacity, "%s", token->text);
        return;
    }
    const field *f = &fields[token->field_index];
    if (f->kind == F_ACTION)
        snprintf(caption, capacity, "%s", f->label);
    else
        field_value(f, caption, capacity);
}
static int token_width(const sentence_token *token, const char *caption, int px, int available)
{
    int w = width(caption, px);
    if (token->field_index >= 0) {
        const field *f = &fields[token->field_index];
        w += f->value == &rule_edit.color ? 37 : 20;
        if (f->icon) w += RULE_ICON_SIZE + RULE_ICON_GAP;
        if (w < 44) w = 44;
    }
    return w < available ? w : available;
}
static int sentence_layout(az_rect rects[32], int available, int px)
{
    int x = 0, row = 0;
    for (int i = 0; i < sentence_count; ++i) {
        char caption[160];
        const sentence_token *token = &sentence[i];
        token_caption(token, caption, sizeof caption);
        int w = token_width(token, caption, px, available);
        if (x && (token->line_start || x + w > available)) {
            x = 0;
            row++;
        }
        rects[i] = (az_rect){ x, row * 52, w, 44 };
        x += w + 7;
    }
    return row + 1;
}
static void draw_icon_row(az_draw *d, int x, int y, const char *row, uint32_t ink)
{
    for (int end = 0; end < RULE_ICON_SIZE;) {
        int start = end++;
        while (end < RULE_ICON_SIZE && row[end] == row[start]) end++;
        if (row[start] == '#') C->fill_rect(d, x + start, y, end - start, 1, ink);
    }
}
static void draw_rule_icon(az_draw *d, az_rect r, enum rule_icon icon, int shift, uint32_t ink)
{
    int y = r.y + (r.h - RULE_ICON_SIZE) / 2;
    for (int row = 0; row < RULE_ICON_SIZE; row++)
        draw_icon_row(d, r.x + shift, y + row, rule_icons[icon][row], ink);
}
static void draw_color_swatch(az_draw *d, az_rect r, int choice)
{
    if (choice < CUES_NO_COLOR)
        C->fill_rect(d, r.x + 8, r.y + 16, 12, 12, cues_colors[choice]);
    else
        C->draw_rect(d, r.x + 8, r.y + 16, 12, 12, 1, color(AZ_COL_TEXT_DIM));
}
typedef struct {
    enum rule_icon icon;
    int swatch;
} chip_mark;

static int draw_chip_mark(az_draw *d, az_rect r, const char *caption, chip_mark mark, uint32_t ink)
{
    if (mark.swatch >= 0) {
        draw_color_swatch(d, r, mark.swatch);
        return 25;
    }
    if (mark.icon != RULE_ICON_NONE) {
        int x = *caption ? 10 : (r.w - RULE_ICON_SIZE) / 2;
        draw_rule_icon(d, r, mark.icon, x, ink);
        return 10 + RULE_ICON_SIZE + RULE_ICON_GAP;
    }
    return 10;
}
static void draw_chip(az_draw *d, az_rect r, const char *caption, hit target,
                      int active, chip_mark mark, int px)
{
    uint32_t bg = color(AZ_COL_BG), ink = color(AZ_COL_TEXT), accent = color(AZ_COL_HI);
    int focused = focus == hit_count;
    C->fill_rect(d, r.x, r.y, r.w, r.h, blend(bg, active ? accent : ink, active ? 25 : 7));
    C->draw_rect(d, r.x, r.y, r.w, r.h, focused ? 2 : 1,
                 active || focused ? accent : blend(bg, ink, 25));
    int pad = draw_chip_mark(d, r, caption, mark, ink);
    label(d, (az_rect){ r.x + pad, r.y, r.w - pad - 9, r.h }, caption, px, ink);
    hits[hit_count++] = target;
}
static void draw_token(az_draw *d, az_rect r, int i, int px)
{
    const sentence_token *token = &sentence[i];
    if (token->text) {
        label(d, r, token->text, px, color(AZ_COL_TEXT));
        return;
    }
    int index = token->field_index;
    const field *f = &fields[index];
    char caption[160];
    token_caption(token, caption, sizeof caption);
    chip_mark mark = { f->icon, f->value == &rule_edit.color ? rule_edit.color : -1 };
    draw_chip(d, r, caption, (hit){ r, H_FIELD, index }, f->active,
              mark, px);
}
static void draw_sentence(az_draw *d, az_rect body, int px, int visible)
{
    az_rect rects[32];
    int rows = sentence_layout(rects, body.w - 24, px);
    pages = (rows + visible - 1) / visible;
    if (page >= pages) page = pages - 1;
    int top = page * visible * 52;
    int remaining = rows * 52 - top;
    int h = remaining < body.h ? remaining : body.h;
    C->fill_rect(d, body.x, body.y, body.w, h, blend(color(AZ_COL_BG), color(AZ_COL_TEXT), 5));
    for (int i = 0; i < sentence_count; ++i) {
        az_rect r = rects[i];
        int y = r.y - top;
        if (y >= 0 && y < visible * 52)
            draw_token(d, (az_rect){ body.x + 12 + r.x, body.y + 4 + y, r.w, r.h }, i, px);
    }
}
static void editor_heading(az_draw *d, az_rect a, const char *heading, const char *detail)
{
    int detail_w = a.w >= 480 ? a.w / 2 : 0;
    label(d, (az_rect){ a.x, a.y, a.w - detail_w, 44 }, heading, a.w < 320 ? 15 : 20, color(AZ_COL_TEXT));
    if (detail_w)
        label(d, (az_rect){ a.x + a.w - detail_w, a.y, detail_w, 44 }, detail, 14, color(AZ_COL_TEXT_DIM));
}
enum { EDITOR_CANCEL_W = 104, EDITOR_SAVE_W = 132, EDITOR_ACTION_GAP = 8 };
static int editor_cancel_width(az_rect a)
{
    return a.w >= 360 ? EDITOR_CANCEL_W : 66;
}
static void draw_editor_pager(az_draw *d, az_rect a, int current, int total)
{
    if (total <= 1) return;
    int x = a.x + editor_cancel_width(a) + 2, y = a.y + a.h - 48;
    button(d, (az_rect){ x, y, 44, 44 }, "<", H_PAGE, -1, 0);
    button(d, (az_rect){ x + 46, y, 44, 44 }, ">", H_PAGE, 1, 0);
    char position[32];
    snprintf(position, sizeof position, "%d/%d", current + 1, total);
    if (a.w >= 400)
        label(d, (az_rect){ x + 102, y, 48, 44 }, position, 13, color(AZ_COL_TEXT_DIM));
}
static void draw_editor_cancel(az_draw *d, az_rect r)
{
    draw_chip(d, r, r.w == EDITOR_CANCEL_W ? "Cancel" : "", (hit){ r, H_BACK, 0 },
              0, (chip_mark){ RULE_ICON_CANCEL, -1 }, 14);
}
static void draw_rule_buttons(az_draw *d, az_rect a, int wide)
{
    int save_w = a.w >= 360 ? EDITOR_SAVE_W : 58;
    int cancel_w = editor_cancel_width(a), right = a.x + a.w;
    int x = wide ? right - save_w - cancel_w - EDITOR_ACTION_GAP : a.x;
    int y = wide ? a.y : a.y + a.h - 48;
    draw_editor_cancel(d, (az_rect){ x, y, cancel_w, 44 });
    az_rect save = { right - save_w, y, save_w, 44 };
    draw_chip(d, save, a.w >= 360 ? "Save rule" : "", (hit){ save, H_SAVE_RULE, 0 },
              1, (chip_mark){ RULE_ICON_SAVE, -1 }, 14);
}
static void draw_rule_editor(az_draw *d, az_rect a)
{
    int wide = a.w >= 680;
    int buttons = EDITOR_CANCEL_W + EDITOR_SAVE_W + 2 * EDITOR_ACTION_GAP;
    int help_x = a.x + a.w - (wide ? buttons : 0) - 64;
    az_rect heading = { a.x, a.y, help_x - a.x - 8, 44 };
    editor_heading(d, heading, title(), config.groups[group_index].name);
    button(d, (az_rect){ help_x, a.y, 64, 44 }, "Help", H_TAB, TAB_HELP, 0);
    int visible = (a.h - 100) / 52;
    draw_sentence(d, (az_rect){ a.x, a.y + 46, a.w, visible * 52 }, a.w < 480 ? 13 : 16, visible);
    draw_rule_buttons(d, a, wide);
    draw_editor_pager(d, a, page, pages);
}
static void draw_rule_choices(az_draw *d, az_rect a)
{
    const field *f = &fields[rule_choice];
    editor_heading(d, a, f->label, title());
    int cols = a.w / 192, rows = (a.h - 100) / 52;
    int per = cols * rows, w = (a.w - (cols - 1) * 6) / cols;
    choice_pages = (f->count + per - 1) / per;
    if (choice_page >= choice_pages) choice_page = choice_pages - 1;
    for (int i = 0; i < per && i + choice_page * per < f->count; ++i) {
        int option = i + choice_page * per;
        int value = f->min + option;
        az_rect r = { a.x + i % cols * (w + 6), a.y + 46 + i / cols * 52, w, 44 };
        chip_mark mark = { RULE_ICON_NONE, f->value == &rule_edit.color ? value : -1 };
        draw_chip(d, r, f->choices[option], (hit){ r, H_CHOICE, value }, value == *(int *)f->value,
                  mark, 15);
    }
    draw_editor_cancel(d, (az_rect){ a.x, a.y + a.h - 48, editor_cancel_width(a), 44 });
    draw_editor_pager(d, a, choice_page, choice_pages);
}

static void draw_action_row(az_draw *d, int left, int y, int content, int i, const field *f)
{
    char value[192];
    snprintf(value, sizeof value, "%s", f->label);
    if (view == GROUPS && f->action == A_OPEN_GROUP)
        snprintf(value, sizeof value, "%s  /  %d rules%s", f->label,
                 config.groups[f->arg].rule_count,
                 config.groups[f->arg].enabled ? "" : "  (off)");
    button(d, (az_rect){ left, y, content, 44 }, value, H_FIELD, i, 0);
}
static void draw_field_row(az_draw *d, int left, int y, int content, int compact, int i)
{
    field f = fields[i];
    if (f.kind == F_ACTION) {
        draw_action_row(d, left, y, content, i, &f);
        return;
    }
    int lw = compact ? content * 44 / 100 : content * 42 / 100;
    label(d, (az_rect){ left, y, lw - 10, 44 }, f.label, compact ? 12 : 15,
          color(AZ_COL_TEXT_DIM));
    char value[160];
    field_value(&f, value, sizeof value);
    button(d, (az_rect){ left + lw, y, content - lw, 44 }, value, H_FIELD, i, 0);
}
static void draw_rows(az_draw *d, int left, int content, int compact, int row_top, int row_h, int visible)
{
    for (int i = page * visible; i < field_count && i < (page + 1) * visible; i++)
        draw_field_row(d, left, row_top + (i - page * visible) * row_h, content, compact, i);
}

static int selected_tab(void)
{
    return view == GLOBAL ? TAB_CONFIG : view == FILES ? TAB_FILES : TAB_GROUPS;
}
static void draw_tab(az_draw *d, az_rect r, int tab, int active)
{
    const char *caption = tabs[tab].label;
    if (r.w >= 68) {
        button(d, r, caption, H_TAB, tab, active);
        return;
    }
    button(d, r, "", H_TAB, tab, active);
    int w = width(caption, 11);
    if (w > r.w - 4) w = r.w - 4;
    label(d, (az_rect){ r.x + (r.w - w) / 2, r.y, w, r.h }, caption, 11,
          color(active ? AZ_COL_HI_TEXT : AZ_COL_TEXT));
}
static void draw_tabs(az_draw *d, int left, int top, int content)
{
    int gap = 4, gaps = (TAB_COUNT - 1) * gap;
    int w = content >= 730 ? 152 : (content - gaps) / TAB_COUNT;
    int x = left + content - w * TAB_COUNT - gaps;
    if (content >= 730)
        label(d, (az_rect){ left, top, x - left - 16, 44 }, "Cues", 26, color(AZ_COL_TEXT));
    int active = selected_tab();
    for (int i = 0; i < TAB_COUNT; i++)
        draw_tab(d, (az_rect){ x + i * (w + gap), top, w, 44 }, i, i == active);
}

static void draw_title(az_draw *d, az_rect a, int left, int content, int compact, int pages)
{
    label(d, (az_rect){ left, a.y + 51, compact ? content - 60 : content, 24 }, title(), 16,
          color(AZ_COL_TEXT));
    if (compact) {
        char count[24];
        snprintf(count, sizeof count, "%d/%d", page + 1, pages);
        label(d, (az_rect){ left + content - 48, a.y + 51, 48, 24 }, count, 12,
              color(AZ_COL_TEXT_DIM));
    }
}

static void draw_footer(az_draw *d, az_rect a, int left, int content, int compact, int pages)
{
    int fy = a.y + a.h - 48, bw = compact ? 52 : 92;
    button(d, (az_rect){ left, fy, bw, 44 }, "Back", H_BACK, 0, 0);
    button(d, (az_rect){ left + content - 2 * bw - 4, fy, bw, 44 }, "<", H_PAGE, -1, 0);
    button(d, (az_rect){ left + content - bw, fy, bw, 44 }, ">", H_PAGE, 1, 0);
    char summary[320];
    snprintf(summary, sizeof summary, "%d/%d  %s", page + 1, pages,
             draft_pending ? "Draft changes; Apply when ready" : storage_status);
    int statusw = content - 3 * bw - 20;
    if (statusw > 40)
        label(d, (az_rect){ left + bw + 10, fy, statusw, 22 }, summary, 11, color(AZ_COL_TEXT_DIM));
    if (statusw > 40)
        label(d, (az_rect){ left + bw + 10, fy + 22, statusw, 22 },
              runtime_status[0]       ? runtime_status
              : active_config.enabled ? "Preserves existing cues"
                                      : "Automatic placement off",
              11, color(AZ_COL_TEXT_DIM));
}

static int visible_rows(az_rect bounds)
{
    int visible = (bounds.h - 140) / 52;
    return visible < 1 ? 1 : visible;
}
static int field_pages(int visible)
{
    int pages = (field_count + visible - 1) / visible;
    return pages < 1 ? 1 : pages;
}
typedef struct {
    int font_px, line_h, text_w, edit_w;
} rule_card_style;
typedef struct {
    int page, y, height;
} rule_card;

static size_t utf8_boundary(const char *text, size_t end)
{
    while (end && ((unsigned char)text[end] & 0xc0) == 0x80)
        end--;
    return end;
}
static int prefix_width(char *text, size_t end, int px)
{
    char saved = text[end];
    text[end] = 0;
    int w = width(text, px);
    text[end] = saved;
    return w;
}
static size_t fitting_prefix(char *text, int px, int w)
{
    size_t first = 0, last = strlen(text);
    if (width(text, px) <= w) return last;
    while (first < last) {
        size_t mid = first + (last - first + 1) / 2;
        size_t end = utf8_boundary(text, mid);
        if (prefix_width(text, end, px) <= w)
            first = mid;
        else
            last = mid - 1;
    }
    return utf8_boundary(text, first);
}
static size_t wrapped_prefix(char *text, int px, int w)
{
    size_t length = fitting_prefix(text, px, w);
    if (!text[length] || text[length] == ' ') return length;
    for (size_t end = length; end > 0; end--)
        if (text[end - 1] == ' ') return end - 1;
    return length;
}
static char *next_summary_line(char *text, size_t length)
{
    char *next = text + length;
    while (*next == ' ') next++;
    return next;
}
static void rule_summary(char summary[512], const cues_rule *rule)
{
    cues_rule_sentence(rule, summary, 512);
    for (unsigned char *p = (unsigned char *)summary; *p; p++)
        if (*p < 32) *p = ' ';
}
static int rule_card_height(const cues_rule *rule, const rule_card_style *style, int available)
{
    char summary[512];
    rule_summary(summary, rule);
    int height = 0, step = 2 * style->line_h;
    for (char *line = summary; *line && height + step <= available; height += step)
        line = next_summary_line(line, wrapped_prefix(line, style->font_px, style->text_w));
    return height;
}
static void draw_summary_line(az_draw *d, az_rect r, char *line, size_t length,
                              int px, uint32_t text, int clipped)
{
    if (clipped) {
        size_t end = fitting_prefix(line, px, r.w - width("...", px));
        char clipped_line[512];
        snprintf(clipped_line, sizeof clipped_line, "%.*s...", (int)end, line);
        C->draw_text(d, r.x, r.y, clipped_line, px, text);
        return;
    }
    char saved = line[length];
    line[length] = 0;
    C->draw_text(d, r.x, r.y, line, px, text);
    line[length] = saved;
}
static void draw_rule_summary(az_draw *d, az_rect r, const cues_rule *rule, const rule_card_style *style)
{
    char summary[512];
    rule_summary(summary, rule);
    char *line = summary;
    az_rect text_area = { r.x + 12, r.y + r.h / 4, style->text_w, style->line_h };
    int rows = r.h / (2 * style->line_h);
    uint32_t text = color(rule->enabled ? AZ_COL_TEXT : AZ_COL_TEXT_DIM);
    for (int i = 0; i < rows; ++i) {
        size_t length = wrapped_prefix(line, style->font_px, style->text_w);
        char *next = next_summary_line(line, length);
        draw_summary_line(d, text_area, line, length, style->font_px, text, i == rows - 1 && *next);
        text_area.y += style->line_h;
        line = next;
    }
}
static void draw_rule_list_actions(az_draw *d, az_rect a, int y, int count)
{
    int w = (a.w - (count - 1) * 6) / count;
    const char *const compact[] = { "Apply", "Group", "+ Rule" };
    for (int i = 0; i < count; ++i) {
        const char *caption = a.w < 480 ? compact[i] : fields[i].label;
        button(d, (az_rect){ a.x + i * (w + 6), y, w, 44 }, caption, H_FIELD, i,
               i == 0 && draft_pending);
    }
}
static int layout_rule_cards(rule_card cards[CUES_MAX_RULES], const cues_group *g,
                             const rule_card_style *style, int available)
{
    int y = 0, current = 0;
    for (int i = 0; i < g->rule_count; ++i) {
        int height = rule_card_height(&g->rules[i], style, available);
        if (y + height > available) {
            y = 0;
            current++;
        }
        cards[i] = (rule_card){ current, y, height };
        y += height + 4;
    }
    return current + 1;
}
static void draw_rule_card(az_draw *d, az_rect r, const cues_rule *rule,
                           const rule_card_style *style, int index, int field_index)
{
    uint32_t bg = color(AZ_COL_BG), text = color(AZ_COL_TEXT);
    uint32_t border = focus == hit_count ? color(AZ_COL_HI) : blend(bg, text, 28);
    C->fill_rect(d, r.x, r.y, r.w, r.h, blend(bg, text, index % 2 ? 17 : 7));
    C->draw_rect(d, r.x, r.y, r.w, r.h, 1, border);
    hits[hit_count++] = (hit){ r, H_FIELD, field_index };
    if (style->edit_w)
        label(d, (az_rect){ r.x + r.w - style->edit_w, r.y, style->edit_w - 12, r.h },
              "Edit", 14, color(AZ_COL_HI));
    draw_rule_summary(d, r, rule, style);
}
static void draw_rule_cards(az_draw *d, az_rect body, const cues_group *g, int first_field)
{
    int px = body.w < 400 ? 15 : 16, edit_w = body.w >= 480 ? 64 : 0;
    rule_card_style style = { px, px * 3 / 2, body.w - edit_w - 24, edit_w };
    rule_card cards[CUES_MAX_RULES];
    pages = layout_rule_cards(cards, g, &style, body.h);
    if (page >= pages) page = pages - 1;
    for (int i = 0; i < g->rule_count; ++i) {
        if (cards[i].page != page) continue;
        az_rect r = { body.x, body.y + cards[i].y, body.w, cards[i].height };
        draw_rule_card(d, r, &g->rules[i], &style, i, first_field + i);
    }
    if (!g->rule_count)
        label(d, (az_rect){ body.x + 12, body.y, body.w - 24, 44 }, "No rules yet", 14, color(AZ_COL_TEXT_DIM));
}
static void draw_rule_list(az_draw *d, az_rect outer, az_rect a)
{
    int compact = outer.w < 640, header = a.y;
    if (a.h >= 300) {
        draw_tabs(d, a.x, a.y + 4, a.w);
        header += 50;
    }
    const cues_group *g = &config.groups[group_index];
    label(d, (az_rect){ a.x, header + 4, a.w, 30 }, g->name, 18, color(AZ_COL_TEXT));
    int first_field = field_count - g->rule_count;
    draw_rule_list_actions(d, a, header + 36, first_field);
    int top = header + 84;
    draw_rule_cards(d, (az_rect){ a.x, top, a.w, a.y + a.h - 48 - top }, g, first_field);
    draw_footer(d, outer, a.x, a.w, compact, pages);
}
static void draw_settings_view(az_draw *d, az_rect outer, az_rect a)
{
    int compact = outer.w < 640, visible = visible_rows(outer);
    pages = field_pages(visible);
    if (page >= pages) page = pages - 1;
    draw_tabs(d, a.x, a.y + 4, a.w);
    draw_title(d, outer, a.x, a.w, compact, pages);
    draw_rows(d, a.x, a.w, compact, a.y + 82, 52, visible);
    draw_footer(d, outer, a.x, a.w, compact, pages);
}
typedef struct {
    const char *heading;
    const char *body;
} help_step;
static const help_step help_steps[] = {
    {
        "Start a rule",
        "In Groups, open a group or choose + Add group. Select + Add rule, then tap a highlighted "
        "value or select it with the browse knob."
    },
    {
        "Choose placement",
        "Choose Phrase to place a cue at a phrase's start, middle, or end. Select its type and "
        "occurrence. Use Offset to move the cue before or after it. Choose Absolute for a position "
        "in bars, beats, or seconds."
    },
    {
        "Set the cue and loop",
        "Choose Hot cue or Memory cue. Hot cues for all occurrences take the next free pad. "
        "By pad assigns a color from the pad. Use Loop to save a loop."
    },
    {
        "Example",
        "Create a Memory cue at the end of all Chorus/Drop phrases. Turn on Offset and set it to -16 bars "
        "to place the cue 16 bars before the end of each chorus."
    },
    {
        "Save and run",
        "Choose Save rule, then Apply changes to save the rules to USB and activate them. Press CUES on "
        "the waveform and pick a deck. Turn on Apply on track load in Config to run on tracks with no saved cues."
    },
};
typedef struct {
    az_draw *draw;
    az_rect body;
    int px, line_h, current, y;
} help_layout;
static void help_new_page(help_layout *layout)
{
    layout->current++;
    layout->y = 0;
}
static void help_line(help_layout *layout, const char *text, int role)
{
    if (layout->y + layout->line_h > layout->body.h)
        help_new_page(layout);
    if (layout->draw && layout->current == page) {
        az_rect row = { layout->body.x, layout->body.y + layout->y, layout->body.w, layout->line_h };
        label(layout->draw, row, text, layout->px, color(role));
    }
    layout->y += layout->line_h;
}
static void help_paragraph(help_layout *layout, const char *body)
{
    char text[320];
    snprintf(text, sizeof text, "%s", body);
    char *line = text;
    while (*line) {
        size_t length = wrapped_prefix(line, layout->px, layout->body.w);
        if (!length) return;
        char *next = next_summary_line(line, length);
        char saved = line[length];
        line[length] = 0;
        help_line(layout, line, AZ_COL_TEXT);
        line[length] = saved;
        line = next;
    }
}
static void layout_help_step(help_layout *layout, const help_step *step)
{
    if (layout->y) {
        if (layout->y + 8 + 2 * layout->line_h > layout->body.h)
            help_new_page(layout);
        else
            layout->y += 8;
    }
    help_line(layout, step->heading, AZ_COL_HI);
    help_paragraph(layout, step->body);
}
static int layout_help(az_draw *d, az_rect body)
{
    int px = body.w < 420 ? 14 : 15;
    help_layout layout = { .draw = d, .body = body, .px = px, .line_h = 3 * px / 2 + 2 };
    for (size_t i = 0; i < sizeof help_steps / sizeof help_steps[0]; i++)
        layout_help_step(&layout, &help_steps[i]);
    return layout.current + 1;
}
static void draw_help_footer(az_draw *d, az_rect a)
{
    int y = a.y + a.h - 48;
    button(d, (az_rect){ a.x, y, editor_cancel_width(a), 44 }, "Back", H_BACK, 0, 0);
    draw_editor_pager(d, a, page, pages);
    if (pages > 1 && a.w < 400) {
        char position[24];
        snprintf(position, sizeof position, "%d/%d", page + 1, pages);
        label(d, (az_rect){ a.x + a.w - 52, y, 52, 44 }, position, 12, color(AZ_COL_TEXT_DIM));
    }
}
static void draw_help(az_draw *d, az_rect a)
{
    label(d, (az_rect){ a.x, a.y + 4, a.w, 44 }, "Create cue rules", a.w < 320 ? 15 : 22,
          color(AZ_COL_TEXT));
    int w = a.w > 1080 ? 1056 : a.w - 24;
    az_rect body = { a.x + 4, a.y + 52, w, a.h - 104 };
    pages = layout_help(NULL, body);
    if (page >= pages) page = pages - 1;
    layout_help(d, body);
    draw_help_footer(d, a);
}
static void draw_page(az_draw *d, az_rect outer, az_rect a)
{
    if (help_open)
        draw_help(d, a);
    else if (rule_choice >= 0)
        draw_rule_choices(d, a);
    else if (view == RULE)
        draw_rule_editor(d, a);
    else if (view == RULES)
        draw_rule_list(d, outer, a);
    else
        draw_settings_view(d, outer, a);
}
static void draw(az_draw *d, az_rect a, void *unused)
{
    (void)unused;
    area = a;
    hit_count = 0;
    if (!C || a.w < 240 || a.h < 180)
        return;
    C->fill_rect(d, a.x, a.y, a.w, a.h, color(AZ_COL_BG));
    rebuild();
    int content = a.w - 24;
    if (content > 1120) content = 1120;
    draw_page(d, a, (az_rect){ a.x + (a.w - content) / 2, a.y, content, a.h });
    if (focus >= hit_count)
        focus = 0;
}
static int input_hit(const az_event *e, hit *h)
{
    if (e->kind == AZ_EV_CLICK) {
        *h = hits[focus];
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
        *h = hits[found];
    } else
        return 0;
    return 1;
}
static void move_page(int direction)
{
    if (rule_choice >= 0)
        choice_page = (choice_page + direction + choice_pages) % choice_pages;
    else
        page = (page + direction + pages) % pages;
}
static int input_back(void)
{
    if (help_open) {
        close_help();
        return 1;
    }
    if (rule_choice >= 0) {
        rule_choice = -1;
        focus = 0;
        redraw();
        return 1;
    }
    if (view == GROUPS)
        return 0;
    go(parent_view());
    return 1;
}
static int activate_hit(hit h)
{
    switch (h.kind) {
    case H_TAB:
        if (h.arg == TAB_HELP)
            open_help();
        else
            go(tabs[h.arg].view);
        break;
    case H_FIELD:
        change_field(h.arg);
        break;
    case H_PAGE: move_page(h.arg); break;
    case H_BACK: return input_back();
    case H_CHOICE: select_rule_choice(h.arg); break;
    case H_SAVE_RULE: save_rule(); break;
    default: break;
    }
    return 1;
}

static int input_ready(void)
{
    return area.w >= 240 && area.h >= 180 && hit_count;
}
static int input(const az_event *e, void *unused)
{
    (void)unused;
    if (!e || !C)
        return 0;
    if (e->kind == AZ_EV_BACK) {
        return input_back();
    }
    if (!input_ready())
        return 0;
    if (e->kind == AZ_EV_KNOB) {
        focus = (focus + (e->dir < 0 ? -1 : 1) + hit_count) % hit_count;
        C->request_redraw();
        return 1;
    }
    hit h;
    if (!input_hit(e, &h)) return 0;
    if (!activate_hit(h)) return 0;
    redraw();
    return 1;
}
static void enter(void *unused)
{
    (void)unused;
    editing_field = help_open = 0;
    rule_choice = -1;
    view = GROUPS;
    page = focus = hit_count = 0;
    area = (az_rect){ 0 };
}
const az_screen_v1 *cues_settings_init(const az_core_v1 *core, void (*changed)(void))
{
    C = core;
    on_change = changed;
    active_revision = saved_revision = 1;
    saving_revision = failed_revision = 0;
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
void cues_settings_start_storage(void)
{
    if (storage_ok)
        storage_ok = cues_store_start(storage_status, sizeof storage_status);
}
const cues_config *cues_settings_config(void)
{
    return &active_config;
}
void cues_settings_revisions(cues_revisions *revisions)
{
    *revisions = (cues_revisions){
        .active = active_revision, .saved = saved_revision, .saving = saving_revision,
        .failed = failed_revision, .queued = dirty, .available = storage_ok
    };
}

int cues_settings_remote_apply(const cues_config *validated, unsigned expected)
{
    if (expected != active_revision)
        return AZ_STALE;
    if (draft_pending || editing_field || view == RULE || view == DELETE_RULE || view == DELETE_GROUP)
        return AZ_BUSY;
    config = *validated;
    commit_config(&config);
    if (group_index >= config.group_count) {
        view = GROUPS;
        page = focus = hit_count = 0;
    }
    redraw();
    return AZ_OK;
}

int cues_settings_remote_save(unsigned expected)
{
    if (expected != active_revision)
        return AZ_STALE;
    request_save();
    if (saving_revision != active_revision)
        return storage_ok ? AZ_BUSY : AZ_FAILED;
    return AZ_OK;
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
    unsigned completed;
    char status[160];
    int result = cues_store_take(&completed, status, sizeof status);
    if (result) {
        if (saving_revision == completed)
            saving_revision = 0;
        if (result < 0)
            failed_revision = completed;
        snprintf(storage_status, sizeof storage_status, "%s", status);
        if (result > 0) {
            saved_revision = completed;
            dirty = active_revision != saved_revision;
        } else
            /* Do not retry the failed revision automatically, but retain a
             * newer Apply queued while that write was in flight. */
            dirty = active_revision != completed;
        redraw();
    }
    if (dirty && storage_ok) {
        int rc = cues_store_request(&active_config, active_revision);
        if (rc > 0) {
            saving_revision = active_revision;
            dirty = 0;
            snprintf(storage_status, sizeof storage_status, "Saving rules to USB...");
        }
    }
}
static const az_screen_v1 screen = {
    .size = sizeof screen,
    .on_draw = draw,
    .on_input = input,
    .on_enter = enter,
    .flags = AZ_SCREEN_DEDICATED,
};
