/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { HEADER = 54, COL_HEADER = 36, FOOTER = 48, PREVIEW_ROWS = 1, INSET = 16 };
static const int tempo_limits[] = {18, 4, 8, 12};

static void open_page(SpRuntime *runtime, enum SpPage page)
{
    runtime->page = page;
    runtime->cursor = 0;
    runtime->top = 0;
}

static int energy_rows(SpEnergyChoice choice)
{
    switch (choice.mode) {
    case SP_FULL: return 1;
    case SP_PHRASE: return 3;
    default: return 2;
    }
}

static int item_count(const SpRuntime *runtime)
{
    int count = (int)runtime->model.source.identity.total;
    switch (runtime->page) {
    case SP_SETTINGS: return 5;
    case SP_SETUP: return 4;
    case SP_NOTICE: return 0;
    case SP_PICK_START:
    case SP_PICK_END: return count + 1;
    case SP_ORDER: return count + PREVIEW_ROWS;
    case SP_ENERGY: return energy_rows(runtime->energy_choice);
    }
    return 0;
}

static const char *page_title(enum SpPage page)
{
    static const char *const titles[] = {"Set Planner settings", "Plan set",
        "Choose opener", "Choose closer", "Set preview", "Energy source", "Set Planner"};
    return titles[page];
}

static const char *energy_title(enum SpEnergy energy)
{
    static const char *const titles[] = {"No energy", "Full track", "One phrase", "All of a type"};
    return titles[energy];
}

static const char *phrase_title(enum az_phrase_kind kind)
{
    static const char *const names[] = {"Unclassified", "Intro", "Build", "Break",
        "Chorus", "Outro", "Verse", "Bridge"};
    return names[kind];
}

static const char *occurrence_title(enum SpOccurrence occurrence)
{
    static const char *const names[] = {"First", "Middle", "Last"};
    return names[occurrence];
}

typedef struct { const char *label, *value; } SettingRow;

static const char *display_title(const SpTrack *track)
{
    return *track->title ? track->title : "(Untitled track)";
}

static const char *endpoint(const SpModel *model, int index)
{
    return index < 0 ? "Automatic" : display_title(&model->tracks[index]);
}

static const char *toggle_label(bool enabled)
{
    return enabled ? "On" : "Off";
}

static void energy_summary(SpEnergyChoice choice, char *text, size_t size)
{
    switch (choice.mode) {
    case SP_FULL: snprintf(text, size, "Full track"); break;
    case SP_PHRASE:
        snprintf(text, size, "%s %s", occurrence_title(choice.occurrence), phrase_title(choice.kind));
        break;
    default: snprintf(text, size, "All of a type: %s", phrase_title(choice.kind)); break;
    }
}

static SettingRow setting_row(const SpRuntime *runtime, int index, char *text, size_t size)
{
    const SpOptions *options = &runtime->options;
    switch (index) {
    case 0:
        energy_summary(runtime->energy_choice, text, size);
        return (SettingRow){"Energy source", text};
    case 1: return (SettingRow){"Energy curve", sp_curve_names[options->curve]};
    case 2:
        snprintf(text, size, "%.0f BPM", options->max_bpm);
        return (SettingRow){"Tempo jump warning", text};
    case 3: return (SettingRow){"Half / double tempo", toggle_label(options->halftime)};
    default: return (SettingRow){"Use track duration", toggle_label(options->time_aware)};
    }
}

static SettingRow row_content(const SpRuntime *runtime, int index, char *text, size_t size)
{
    const SpModel *model = &runtime->model;
    const SpOptions *options = &runtime->options;
    SpEnergyChoice choice = runtime->energy_choice;
    if (runtime->page == SP_SETTINGS)
        return setting_row(runtime, index, text, size);
    if (runtime->page == SP_ENERGY) {
        SettingRow rows[] = {{"Mode", energy_title(choice.mode)}, {"Phrase type", phrase_title(choice.kind)},
            {"Occurrence", occurrence_title(choice.occurrence)}};
        return rows[index];
    }
    switch (index) {
    case 0: return (SettingRow){"Opener", endpoint(model, options->opener)};
    case 1: return (SettingRow){"Closer", endpoint(model, options->closer)};
    case 2: return (SettingRow){"Preview set", ""};
    default: return (SettingRow){"Refresh analysis and preview", ""};
    }
}

static const char *status_text(const SpRuntime *runtime)
{
    static const struct { int status; const char *message; } messages[] = {
        {AZ_OK, ""},
        {AZ_CANCELLED, "Cancelled."},
        {AZ_EMPTY, "This playlist is empty."},
        {AZ_STALE, "USB or playlist changed. Reopen the playlist."},
        {AZ_BUSY, "The library is busy. Try again."},
        {AZ_LIMIT, "Playlist exceeds the supported size."},
        {AZ_UNSUPPORTED, "Enable Playlist Editing in Mod Menu to save."}
    };
    if (runtime->sync_failed)
        return "USB order verified. Device Library sync failed.";
    for (size_t i = 0; i < sizeof messages / sizeof *messages; ++i)
        if (runtime->result == messages[i].status)
            return messages[i].message;
    return "The deck could not complete the request.";
}

static bool busy(const SpRuntime *runtime)
{
    return atomic_load(&runtime->phase) != SP_IDLE;
}

static const char *job_title(enum SpJob job)
{
    switch (job) {
    case SP_LOAD: return "Reading tracks";
    case SP_APPLY: return "Saving playlist order";
    default: return "Preparing the set";
    }
}

typedef struct {
    const SpRuntime *runtime;
    const az_core_v1 *core;
    az_draw *draw;
    uint32_t bg, stripe, head, rule, select, text, dim, selected_text, action;
} Paint;

static uint32_t mix(uint32_t from, uint32_t to, unsigned percent)
{
    uint32_t result = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        unsigned lo = (from >> shift) & 255, hi = (to >> shift) & 255;
        result |= ((lo * (100 - percent) + hi * percent) / 100) << shift;
    }
    return result;
}

static Paint palette(const SpRuntime *runtime, az_draw *draw)
{
    const az_core_v1 *c = runtime->core;
    uint32_t bg = c->color(AZ_COL_BG), ink = c->color(AZ_COL_TEXT);
    if (c->ui_theme && c->ui_theme() == 0)
        return (Paint){.runtime = runtime, .core = c, .draw = draw, .bg = 0x000000,
            .stripe = 0x262626, .head = 0x383838, .rule = 0x515151, .select = 0x007acc,
            .text = 0xffffff, .dim = 0xbfbfbf, .selected_text = 0xffffff, .action = 0x1074dc};
    return (Paint){.runtime = runtime, .core = c, .draw = draw, .bg = bg,
        .stripe = mix(bg, ink, 5), .head = mix(bg, ink, 12), .rule = mix(bg, ink, 19),
        .select = mix(bg, 0x77849a, 70), .text = ink, .dim = c->color(AZ_COL_TEXT_DIM),
        .selected_text = 0xffffff, .action = 0x1074dc};
}

static void rect(const Paint *p, int x, int y, int w, int h, uint32_t color)
{
    if (w > 0 && h > 0)
        p->core->fill_rect(p->draw, x, y, w, h, color);
}

static void text(const Paint *p, int x, int y, const char *label, int px, uint32_t color)
{
    p->core->draw_text(p->draw, x, y, label, px, color);
}

static uint32_t next_codepoint(const unsigned char **cursor)
{
    unsigned lead = *(*cursor)++;
    if (lead < 0xc2 || lead > 0xf4)
        return lead;
    unsigned remaining = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
    uint32_t codepoint = lead & (0x7fu >> (remaining + 1));
    for (unsigned i = 0; i < remaining; ++i) {
        unsigned byte = **cursor;
        if ((byte & 0xc0) != 0x80)
            return '?';
        ++*cursor;
        codepoint = codepoint << 6 | (byte & 0x3f);
    }
    return codepoint;
}

/* The SDK's menu face takes one Latin-1 byte per glyph. */
static char native_glyph(uint32_t codepoint)
{
    if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint < 0xa0))
        return ' ';
    if (codepoint <= 0xff)
        return (char)codepoint;
    static const struct { uint32_t codepoint; char glyph; } conversions[] = {
        {0x2013, '-'}, {0x2014, '-'}, {0x2018, '\''}, {0x2019, '\''},
        {0x201c, '"'}, {0x201d, '"'}, {0x266d, 'b'}, {0x266f, '#'}
    };
    for (size_t i = 0; i < sizeof conversions / sizeof *conversions; ++i)
        if (conversions[i].codepoint == codepoint)
            return conversions[i].glyph;
    return '?';
}

static void native_label(const char *value, char *out, size_t size)
{
    const unsigned char *cursor = (const unsigned char *)value;
    size_t i = 0;
    while (*cursor && i + 1 < size)
        out[i++] = native_glyph(next_codepoint(&cursor));
    out[i] = 0;
}

static void fit_label(const Paint *p, const char *label, int px, int width, char *out, size_t size)
{
    size_t lo = 0, hi = strlen(label);
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        snprintf(out, size, "%.*s...", (int)mid, label);
        if (p->core->text_width(out, px) <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    snprintf(out, size, "%.*s...", (int)lo, label);
}

static void label(const Paint *p, int x, int y, const char *value, int px, int width, uint32_t color)
{
    if (width < 16)
        return;
    char native[256];
    native_label(value, native, sizeof native);
    if (p->core->text_width(native, px) <= width) {
        text(p, x, y, native, px, color);
        return;
    }
    char fitted[260];
    fit_label(p, native, px, width, fitted, sizeof fitted);
    text(p, x, y, fitted, px, color);
}

static bool table_page(enum SpPage page)
{
    return page == SP_ORDER || page == SP_PICK_START || page == SP_PICK_END;
}

static int row_start(const SpRuntime *runtime)
{
    if (runtime->page == SP_ORDER && runtime->top < PREVIEW_ROWS)
        return PREVIEW_ROWS;
    return runtime->top;
}

static int track_start(const SpRuntime *runtime)
{
    return runtime->area.y + HEADER + (table_page(runtime->page) ? COL_HEADER : 0);
}

static int x_at(const SpRuntime *runtime, int percentage)
{
    return runtime->area.x + runtime->area.w * percentage / 100;
}

static int save_x(const SpRuntime *runtime)
{
    return x_at(runtime, 82);
}

static void draw_save(const Paint *p, az_rect area)
{
    const SpRuntime *runtime = p->runtime;
    const SpModel *model = &runtime->model;
    int x = save_x(runtime), width = area.x + area.w - x - INSET;
    uint32_t color = runtime->cursor == 0 ? p->action : p->head;
    rect(p, x, area.y + 8, width, 38, color);
    char action[80];
    snprintf(action, sizeof action, "%s (%d moves)", model->verified ? "Resume saving" : "Save order",
             model->move_count - model->verified);
    label(p, x + 10, area.y + 14, action, 18, width - 20, runtime->cursor == 0 ? 0xffffff : p->text);
}

static void draw_order_status(const Paint *p, az_rect area)
{
    const SpModel *model = &p->runtime->model;
    if (model->can_resume) {
        draw_save(p, area);
        return;
    }
    int x = save_x(p->runtime);
    char message[80] = "Order already matches";
    if (model->verified)
        snprintf(message, sizeof message, "%d playlist moves verified", model->verified);
    label(p, x, area.y + 14, message, 16, area.x + area.w - x - INSET, p->dim);
}

static void draw_back(const Paint *p, az_rect area)
{
    int x = area.x + INSET, y = area.y + 7;
    rect(p, x, y, 46, 40, p->head);
    rect(p, x + 13, y + 19, 20, 3, p->text);
    for (int step = 0; step < 9; ++step) {
        rect(p, x + 13 + step, y + 19 - step, 3, 2, p->text);
        rect(p, x + 13 + step, y + 20 + step, 3, 2, p->text);
    }
}

static void draw_header(const Paint *p, bool working)
{
    const SpRuntime *runtime = p->runtime;
    az_rect area = runtime->area;
    bool planner = runtime->planner_open;
    int x = area.x + (planner ? 76 : INSET);
    int right = area.x + area.w - INSET;
    rect(p, area.x, area.y, area.w, HEADER, p->bg);
    rect(p, area.x, area.y + HEADER - 1, area.w, 1, p->rule);
    if (planner)
        draw_back(p, area);
    if (!working && runtime->page == SP_ORDER) {
        draw_order_status(p, area);
        right = save_x(runtime) - INSET;
    }
    label(p, x, area.y + 11, page_title(runtime->page), 21, area.w / 5, p->text);
    x += area.w / 5;
    if (!working && planner)
        label(p, x, area.y + 14, runtime->model.playlist_name, 18, right - x, p->dim);
}

static const int columns[] = {1, 7, 54, 76, 85, 92, 100};

static void column(const Paint *p, int index, int y, const char *value, int px, uint32_t color)
{
    const SpRuntime *runtime = p->runtime;
    int x = x_at(runtime, columns[index]);
    label(p, x + 5, y, value, px, x_at(runtime, columns[index + 1]) - x - 10, color);
}

static void draw_columns(const Paint *p)
{
    const SpRuntime *r = p->runtime;
    int y = r->area.y + HEADER;
    rect(p, r->area.x, y, r->area.w, COL_HEADER, p->head);
    const char *headers[] = {"#", "Track", "Artist", "BPM", "Key", "Energy"};
    int count = r->page == SP_ORDER ? 6 : 5;
    for (int i = 0; i < count; ++i) {
        if (i)
            rect(p, x_at(r, columns[i]), y, 1, COL_HEADER, p->rule);
        column(p, i, y + 5, headers[i], 18, p->dim);
    }
}

static void draw_energy(const Paint *p, const SpInput *input, int y, uint32_t color)
{
    char energy[24] = "-";
    if (input->energy != SP_MISSING) {
        bool fallback = input->encoding != AZ_WAVE_PWV7_BANDS ||
            input->energy != p->runtime->energy_choice.mode;
        int score = (int)lround(input->score * 100);
        snprintf(energy, sizeof energy, "%d%s", score, fallback ? "*" : "");
    }
    column(p, 5, y, energy, 22, color);
}

enum { TEMPO_WARNING_COLOR = 0xffc068, KEY_WARNING_COLOR = 0xff8585 };

static void transition_cell(const Paint *p, int col, int y, const char *value, uint32_t color,
                            const char *warning, uint32_t accent)
{
    if (!*warning) {
        column(p, col, y, value, 22, color);
        return;
    }
    column(p, col, y - 6, value, 20, color);
    column(p, col, y + 17, warning, 14, accent);
}

static SpTransition incoming_transition(const SpRuntime *r, int index, const SpTrack *track)
{
    if (r->page != SP_ORDER || index <= PREVIEW_ROWS)
        return (SpTransition){0};
    int previous = r->model.target[index - PREVIEW_ROWS - 1];
    return sp_transition(&r->model.tracks[previous], track, &r->options);
}

static void draw_track_details(const Paint *p, const SpTrack *track, int index, int y, uint32_t color)
{
    const SpRuntime *r = p->runtime;
    SpTransition incoming = incoming_transition(r, index, track);
    char bpm[24] = "-", jump[32] = "";
    if (track->bpm > 0)
        snprintf(bpm, sizeof bpm, "%.1f", track->bpm);
    if (incoming.tempo_warning)
        snprintf(jump, sizeof jump, "Jump %.1f", incoming.bpm_gap);
    if (incoming.tempo_warning || incoming.key_warning) {
        uint32_t accent = incoming.key_warning ? KEY_WARNING_COLOR : TEMPO_WARNING_COLOR;
        int row_y = y - (r->row_height - 33) / 2;
        rect(p, r->area.x, row_y, 4, r->row_height - 1, accent);
    }
    transition_cell(p, 3, y, bpm, color, jump, TEMPO_WARNING_COLOR);
    transition_cell(p, 4, y, *track->key_text ? track->key_text : "-", color,
                    incoming.key_warning ? "Key clash" : "", KEY_WARNING_COLOR);
}

static void draw_track(const Paint *p, int index, int y, uint32_t color)
{
    const SpRuntime *runtime = p->runtime;
    const SpModel *model = &runtime->model;
    bool preview = runtime->page == SP_ORDER;
    if (!preview && index == 0) {
        column(p, 1, y, "Automatic", 22, color);
        return;
    }
    int i = preview ? model->target[index - 1] : index - 1;
    const SpTrack *track = &model->tracks[i];
    char number[24];
    snprintf(number, sizeof number, "%03d", index);
    column(p, 0, y, number, 22, color);
    column(p, 1, y, display_title(track), 22, color);
    column(p, 2, y, track->artist, 22, color);
    draw_track_details(p, track, index, y, color);
    if (preview)
        draw_energy(p, &model->inputs[i], y, color);
}

static void draw_setting(const Paint *p, int index, int y, uint32_t color)
{
    const SpRuntime *runtime = p->runtime;
    char local[96];
    SettingRow row = row_content(runtime, index, local, sizeof local);
    int x = runtime->area.x + INSET;
    int middle = x_at(runtime, 50), right = runtime->area.x + runtime->area.w - INSET;
    label(p, x, y, row.label, 21, middle - x - 12, color);
    label(p, middle, y, row.value, 21, right - middle - 34, color);
    label(p, right - 22, y, ">", 22, 22, color);
    rect(p, middle - 12, y - 5, 1, 31, p->rule);
}

static void draw_scrollbar(const Paint *p, int count)
{
    const SpRuntime *r = p->runtime;
    if (count <= r->visible)
        return;
    int height = r->visible * r->row_height;
    int thumb = (int)sp_clamp((double)height * r->visible / count, 12, height);
    int y = track_start(r) + (height - thumb) * r->top / (count - r->visible);
    rect(p, r->area.x + r->area.w - 4, y, 4, thumb, p->rule);
}

static void draw_rows(const Paint *p)
{
    const SpRuntime *r = p->runtime;
    int count = item_count(r), start = row_start(r), y = track_start(r);
    bool table = table_page(r->page);
    for (int i = start; i < count && i - start < r->visible; ++i, y += r->row_height) {
        bool selected = i == r->cursor;
        uint32_t bg = selected ? p->select : (i % 2 ? p->stripe : p->bg);
        uint32_t fg = selected ? p->selected_text : p->text;
        rect(p, r->area.x, y, r->area.w, r->row_height - 1, bg);
        int text_y = y + (r->row_height - (table ? 33 : 31)) / 2;
        if (table)
            draw_track(p, i, text_y, fg);
        else
            draw_setting(p, i, text_y, fg);
    }
    draw_scrollbar(p, count);
}

static void draw_footer(const Paint *p)
{
    const SpRuntime *r = p->runtime;
    const SpModel *model = &r->model;
    int x = r->area.x + INSET, y = r->area.y + r->area.h - FOOTER;
    int width = r->area.w - 2 * INSET;
    rect(p, r->area.x, y, r->area.w, FOOTER, p->head);
    const char *status = status_text(r);
    if (*status) {
        label(p, x, y + 8, status, 19, width, p->text);
        return;
    }
    if (r->page == SP_ORDER) {
        char metrics[200];
        snprintf(metrics, sizeof metrics, "%u tracks  |  Max jump %.1f BPM  |  %d key warnings  |  %d missing energy  |  %d cached  |  Energy 0-100",
                 model->source.identity.total, model->planned.max_bpm, model->planned.key_warnings,
                 model->unknown_energy, model->cache_hits);
        label(p, x, y + 2, metrics, 17, width, p->text);
        if (model->fallback_energy)
            label(p, x, y + 23, "* Full-track or waveform-height estimate", 15, width, p->dim);
    }
}

static void draw_busy(const Paint *p)
{
    const SpRuntime *r = p->runtime;
    int total = atomic_load(&r->total), progress = atomic_load(&r->progress);
    char line[128];
    snprintf(line, sizeof line, "%s", job_title(r->job));
    if (total)
        snprintf(line, sizeof line, "%s   %d / %d", job_title(r->job), progress, total);
    int x = r->area.x + INSET, y = track_start(r) + INSET, width = r->area.w - 2 * INSET;
    label(p, x, y, line, 23, width, p->text);
    if (total) {
        rect(p, x, y + 43, width, 7, p->head);
        rect(p, x, y + 43, (int)(width * sp_clamp((double)progress / total, 0, 1)), 7, p->action);
    }
    const char *cancel = atomic_load(&r->cancel) ? "Cancelling..." : "BACK to cancel";
    label(p, x, y + 67, cancel, 18, width, p->dim);
}

static void reveal_cursor(SpRuntime *r)
{
    if (r->cursor <= r->top)
        r->top = r->cursor;
    if (r->cursor >= row_start(r) + r->visible)
        r->top = r->cursor - r->visible + 1;
}

static void on_draw(az_draw *draw, az_rect area, void *user)
{
    SpRuntime *r = user;
    r->area = area;
    r->row_height = (int)sp_clamp(r->core->row_height(draw), 42, 54);
    r->visible = (int)sp_clamp((area.y + area.h - FOOTER - track_start(r)) / r->row_height, 1, 32);
    Paint paint = palette(r, draw);
    rect(&paint, area.x, area.y, area.w, area.h, paint.bg);
    bool working = busy(r);
    draw_header(&paint, working);
    if (working) {
        draw_busy(&paint);
        return;
    }
    reveal_cursor(r);
    if (table_page(r->page))
        draw_columns(&paint);
    draw_rows(&paint);
    draw_footer(&paint);
}

static int back(SpRuntime *runtime)
{
    switch (runtime->page) {
    case SP_SETTINGS: return 0;
    case SP_ENERGY: open_page(runtime, SP_SETTINGS); break;
    case SP_SETUP:
    case SP_NOTICE: sp_close_planner(runtime); break;
    case SP_ORDER:
        if (runtime->model.verified)
            sp_close_planner(runtime);
        else
            open_page(runtime, SP_SETUP);
        break;
    default: open_page(runtime, SP_SETUP); break;
    }
    runtime->result = AZ_OK;
    runtime->sync_failed = false;
    return 1;
}

static void cycle_tempo(SpOptions *options)
{
    for (size_t i = 0; i < sizeof tempo_limits / sizeof *tempo_limits; ++i) {
        if (options->max_bpm == tempo_limits[i]) {
            options->max_bpm = tempo_limits[(i + 1) % (sizeof tempo_limits / sizeof *tempo_limits)];
            return;
        }
    }
}

static void settings_select(SpRuntime *runtime)
{
    SpOptions *options = &runtime->options;
    switch (runtime->cursor) {
    case 0: open_page(runtime, SP_ENERGY); break;
    case 1:
        options->curve = (options->curve + 1) % SP_CURVES;
        runtime->core->cfg_set(SP_ID, "curve", options->curve);
        break;
    case 2:
        cycle_tempo(options);
        runtime->core->cfg_set(SP_ID, "tempo-limit", (int)options->max_bpm);
        break;
    case 3:
        options->halftime = !options->halftime;
        runtime->core->cfg_set(SP_ID, "half-tempo", options->halftime);
        break;
    case 4:
        options->time_aware = !options->time_aware;
        runtime->core->cfg_set(SP_ID, "time-aware", options->time_aware);
        break;
    }
}

static void setup_select(SpRuntime *runtime)
{
    switch (runtime->cursor) {
    case 0: open_page(runtime, SP_PICK_START); break;
    case 1: open_page(runtime, SP_PICK_END); break;
    case 2: sp_post_job(runtime, SP_PREVIEW); break;
    case 3: sp_post_job(runtime, SP_REFRESH); break;
    }
}

static void choose_energy(SpRuntime *runtime)
{
    SpEnergyChoice *choice = &runtime->energy_choice;
    switch (runtime->cursor) {
    case 0:
        choice->mode = (choice->mode % (SP_ENERGY_COUNT - 1)) + 1;
        runtime->core->cfg_set(SP_ID, "energy-mode", choice->mode);
        break;
    case 1:
        choice->kind = (choice->kind % AZ_PHRASE_BRIDGE) + 1;
        runtime->core->cfg_set(SP_ID, "phrase-type", choice->kind);
        break;
    case 2:
        choice->occurrence = (choice->occurrence + 1) % SP_OCCURRENCE_COUNT;
        runtime->core->cfg_set(SP_ID, "occurrence", choice->occurrence);
        break;
    }
}

static void choose_endpoint(SpRuntime *runtime)
{
    SpOptions *options = &runtime->options;
    int *picked = runtime->page == SP_PICK_START ? &options->opener : &options->closer;
    int *other = runtime->page == SP_PICK_START ? &options->closer : &options->opener;
    *picked = runtime->cursor - 1;
    if (*other == *picked)
        *other = -1;
    open_page(runtime, SP_SETUP);
}

static void confirm_save(int yes, void *user)
{
    SpRuntime *runtime = user;
    runtime->confirmation_pending = false;
    if (yes && runtime->planner_open)
        sp_post_job(runtime, SP_APPLY);
    sp_redraw(runtime);
}

static void open_confirmation(SpRuntime *runtime)
{
    if (runtime->cursor || !runtime->model.can_resume || runtime->confirmation_pending)
        return;
    char message[160], playlist[96];
    native_label(runtime->model.playlist_name, playlist, sizeof playlist);
    snprintf(message, sizeof message, "Save the reviewed order to \"%.95s\" on USB %d?",
             playlist, runtime->model.stick + 1);
    runtime->confirmation_pending = runtime->core->confirm(SP_ID, "Save playlist order?",
        message, "Save order", confirm_save, runtime) == 0;
}

static void select_row(SpRuntime *runtime)
{
    if (!item_count(runtime))
        return;
    switch (runtime->page) {
    case SP_SETTINGS: settings_select(runtime); break;
    case SP_NOTICE: break;
    case SP_SETUP: setup_select(runtime); break;
    case SP_PICK_START:
    case SP_PICK_END: choose_endpoint(runtime); break;
    case SP_ORDER: open_confirmation(runtime); break;
    case SP_ENERGY: choose_energy(runtime); break;
    }
}

static void move_cursor(SpRuntime *runtime, int index)
{
    runtime->cursor = (int)sp_clamp(index, 0, item_count(runtime) > 0 ? item_count(runtime) - 1 : 0);
}

static void tap_header(SpRuntime *r, int x)
{
    if (r->planner_open && x < r->area.x + 70) {
        back(r);
        return;
    }
    if (r->page == SP_ORDER && x >= save_x(r)) {
        move_cursor(r, 0);
        open_confirmation(r);
    }
}

static void tap(SpRuntime *r, const az_event *event)
{
    if (event->y < r->area.y + HEADER) {
        tap_header(r, event->x);
        return;
    }
    int y = event->y - track_start(r);
    if (y < 0 || y >= r->visible * r->row_height)
        return;
    int index = row_start(r) + y / r->row_height;
    if (index < item_count(r)) {
        move_cursor(r, index);
        select_row(r);
    }
}

static int on_input(const az_event *event, void *user)
{
    SpRuntime *runtime = user;
    if (busy(runtime)) {
        if (event->kind == AZ_EV_BACK)
            atomic_store(&runtime->cancel, true);
    } else {
        switch (event->kind) {
        case AZ_EV_BACK:
            if (!back(runtime))
                return 0;
            break;
        case AZ_EV_KNOB: move_cursor(runtime, runtime->cursor + event->dir); break;
        case AZ_EV_CLICK: select_row(runtime); break;
        case AZ_EV_TAP: tap(runtime, event); break;
        default: return 0;
        }
    }
    sp_redraw(runtime);
    return 1;
}

static void completed(SpRuntime *runtime)
{
    if (runtime->result != AZ_OK)
        return;
    if (runtime->job != SP_APPLY)
        runtime->sync_failed = false;
    switch (runtime->job) {
    case SP_LOAD: open_page(runtime, SP_SETUP); break;
    case SP_PREVIEW:
    case SP_REFRESH: open_page(runtime, SP_ORDER); break;
    default: break;
    }
}

void sp_ui_tick(SpRuntime *runtime)
{
    enum SpPhase phase = atomic_load(&runtime->phase);
    if (phase == SP_DONE) {
        completed(runtime);
        atomic_store(&runtime->phase, SP_IDLE);
        sp_redraw(runtime);
    } else if (phase == SP_WORKING) {
        int progress = atomic_load(&runtime->progress);
        if (progress != runtime->progress_seen) {
            runtime->progress_seen = progress;
            sp_redraw(runtime);
        }
    }
}

const az_screen_v1 sp_screen = {.size = sizeof sp_screen, .on_draw = on_draw,
    .on_input = on_input, .flags = AZ_SCREEN_DEDICATED};


const az_surface_v1 sp_surface = {.size = sizeof sp_surface, .layer = AZ_LAYER_OVER_EP147,
    .views = AZ_VIEW_BIT(AZ_VIEW_BROWSE), .on_paint = on_draw, .on_input = on_input};
