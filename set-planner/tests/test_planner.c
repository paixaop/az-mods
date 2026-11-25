/* SPDX-License-Identifier: GPL-3.0-only */
#define _XOPEN_SOURCE 700
#include "mod.h"
#include <assert.h>
#include <dirent.h>
#include <ftw.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { LARGE = 135, SMALL = 7, TEST_COLUMNS = 12000 };
static az_playlist_entry_v1 entries[LARGE];
static uint32_t length;
static uint64_t revision, generation;
static const az_screen_v1 *registered;
static const az_surface_v1 *planner;
static az_plan_set_fn menu_action;
static void *menu_user;
static az_rect planner_area;
static bool planner_visible, planner_focus, focus_refused, stale_target;
static int current_view = AZ_VIEW_BROWSE;
static unsigned cfg_changes;
static pthread_t worker;
static char drive[512], data[512];
static bool have_data = true, wave_missing, busy_release, busy_order;
static bool changed_page, bad_row, bad_tracks, no_phrases, cancel_wave;
static bool roster_mismatch, wave_invalid, short_map, unfinished_wave, silent_wave, long_title;
static uint32_t wave_format;
static unsigned requests, waves, moves, fail_sync, redraws, pending;
static az_playlist_move_v1 saved;
static uint64_t receipt, tracked_ticket;
static void (*confirmation)(int, void *);
static void *confirmation_user;
static uint32_t wave_first[64], wave_columns[64];
static az_wave_query last_query;
static char drawn[4096];
static double test_bpm;
static unsigned current_track;
static SpRuntime *running;

static void fixture(uint32_t count)
{
    length = count;
    revision = 1;
    generation = 2;
    receipt = 0;
    requests = waves = moves = fail_sync = 0;
    wave_missing = busy_release = busy_order = false;
    changed_page = bad_row = bad_tracks = no_phrases = cancel_wave = false;
    roster_mismatch = wave_invalid = short_map = unfinished_wave = false;
    silent_wave = long_title = false;
    wave_format = AZ_WAVE_PWV7_BANDS;
    test_bpm = 0;
    static const uint32_t tracks[] = {3, 1, 5, 1, 4, 2, 6};
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = count == SMALL ? tracks[i] : i + 1;
        entries[i] = (az_playlist_entry_v1){.entry_id = 100 + i, .track_id = id, .position = i + 1};
    }
}

static int playlists(const char *id, int stick, int (*fn)(int, int, const char *, int, void *),
                     void *user)
{
    assert(strcmp(id, SP_ID) == 0);
    if (stick == 1)
        return AZ_NOT_FOUND;
    if (fn(31, 999, "Sets", 1, user))
        return 0;
    if (fn(44, 31, "Party", 0, user))
        return 0;
    if (fn(30, 0, NULL, 0, user))
        return 0;
    return 1;
}

static int tracks(const char *id, int stick, int playlist,
                  int (*fn)(const az_track *, void *), void *user)
{
    assert(strcmp(id, SP_ID) == 0 && stick == 0 && playlist > 0);
    if (bad_tracks)
        return AZ_NOT_FOUND;
    static const char *keys[] = {"", "8A", "B♭ minor", "F♯", "Abmaj", "c#min", "D minor"};
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t track = entries[i].track_id;
        char title[160], path[700];
        snprintf(title, sizeof title, "Test track %u", track);
        if (long_title && track == 3)
            snprintf(title, sizeof title, "Long title with multiple artists and a prolonged mix name · Extended edição");
        snprintf(path, sizeof path, "%s/Contents/track%u.aiff", drive, track);
        az_track info = {.size = sizeof info, .id = track, .title = title,
            .artist = track == 3 ? "Edição" : track % 2 ? "Artist" : NULL,
            .key = track < 7 ? keys[track] : NULL, .path = path,
            .bpm = track == 1 && test_bpm > 0 ? test_bpm : 98.0 + track * 3,
            .len_s = 80};
        if (roster_mismatch && i == 1)
            ++info.id;
        if (fn(&info, user))
            break;
    }
    return 0;
}

static int order(const char *id, int stick, uint32_t list, uint32_t offset,
                 az_playlist_entry_v1 *rows, uint32_t capacity, az_playlist_order_v1 *out)
{
    assert(strcmp(id, SP_ID) == 0 && stick == 0 && list > 0);
    if (busy_order) {
        busy_order = false;
        return AZ_BUSY;
    }
    *out = (az_playlist_order_v1){.size = sizeof *out, .version = AZ_DATA_VERSION,
        .total = length, .source_generation = generation, .library_id = 9,
        .revision = revision + (changed_page && offset > 0)};
    for (; offset < length && out->returned < capacity; ++offset) {
        az_playlist_entry_v1 row = entries[offset];
        row.position = bad_row ? 0 : offset + 1;
        rows[out->returned++] = row;
    }
    return AZ_OK;
}

static int resolve(const char *id, int stick, uint32_t track, az_track_ref *out)
{
    assert(strcmp(id, SP_ID) == 0 && stick == 0);
    *out = (az_track_ref){.size = sizeof *out, .version = AZ_DATA_VERSION,
        .source_generation = generation, .source_id = 1, .library_id = 9, .track_id = track};
    current_track = track;
    return AZ_OK;
}

static int request(const char *id, const az_track_ref *track, uint32_t parts, uint64_t *ticket)
{
    assert(strcmp(id, SP_ID) == 0 && track->track_id == current_track);
    assert(parts == (AZ_DATA_GRID | AZ_DATA_PHRASES));
    ++requests;
    pending = requests == 1;
    *ticket = ++tracked_ticket;
    return AZ_PENDING;
}

static int acquire(const char *id, uint64_t ticket, const az_track_snapshot **out)
{
    assert(strcmp(id, SP_ID) == 0 && ticket == tracked_ticket);
    if (pending) {
        --pending;
        return AZ_PENDING;
    }
    static az_beat beats[160];
    for (unsigned i = 0; i < 160; ++i)
        beats[i].time_s = i * 0.5;
    static const az_phrase phrases[] = {
        {.start_beat = 0, .end_beat = 1, .category = AZ_PHRASE_INTRO},
        {.start_beat = 1, .end_beat = 7, .raw_mood = 1, .raw_kind = 5},
        {.start_beat = 7, .end_beat = 15, .category = AZ_PHRASE_CHORUS},
        {.start_beat = 15, .end_beat = 35, .category = AZ_PHRASE_BUILD},
        {.start_beat = 34, .end_beat = 40, .category = AZ_PHRASE_CHORUS},
        {.start_beat = 44, .end_beat = 44, .category = AZ_PHRASE_VERSE},
        {.start_beat = 35, .end_beat = 46, .category = AZ_PHRASE_CHORUS},
        {.start_beat = 60, .end_beat = 71, .category = AZ_PHRASE_CHORUS},
        {.start_beat = 120, .end_beat = 160, .category = AZ_PHRASE_CHORUS},
    };
    static az_track_snapshot view;
    view = (az_track_snapshot){.grid_info.availability = AZ_AVAIL_READY,
        .phrases_info.availability = no_phrases ? AZ_AVAIL_EMPTY : AZ_AVAIL_READY,
        .beats = beats, .beat_count = short_map ? 0 : 160, .phrases = phrases,
        .phrase_count = sizeof phrases / sizeof *phrases};
    *out = &view;
    return AZ_OK;
}

static int release(const char *id, uint64_t ticket)
{
    assert(strcmp(id, SP_ID) == 0 && ticket == tracked_ticket);
    if (busy_release) {
        busy_release = false;
        return AZ_BUSY;
    }
    return AZ_OK;
}

static int wave_request(const char *id, const az_track_ref *track, const az_wave_query *query,
                        uint64_t *ticket)
{
    assert(strcmp(id, SP_ID) == 0 && track->track_id == current_track);
    assert(query->column_count <= AZ_WAVE_WINDOW_MAX);
    if (query->encoding != wave_format || wave_missing)
        return AZ_NOT_FOUND;
    wave_first[waves % 64] = query->first_column;
    wave_columns[waves % 64] = query->column_count;
    ++waves;
    last_query = *query;
    pending = waves == 1;
    if (cancel_wave) {
        atomic_store(&running->cancel, true);
        return AZ_BUSY;
    }
    *ticket = ++tracked_ticket;
    return AZ_PENDING;
}

static int wave_acquire(const char *id, uint64_t ticket, const az_wave_snapshot **out)
{
    assert(strcmp(id, SP_ID) == 0 && ticket == tracked_ticket);
    if (pending) {
        --pending;
        return AZ_BUSY;
    }
    unsigned first = last_query.first_column;
    uint32_t total = TEST_COLUMNS;
    if (first >= total)
        return AZ_EMPTY;
    unsigned count = last_query.column_count;
    if (count > total - first)
        count = total - first;
    static uint8_t bytes[AZ_WAVE_WINDOW_MAX * 3];
    unsigned stride = wave_format == AZ_WAVE_PWV7_BANDS ? 3 :
        wave_format == AZ_WAVE_PWV5_RGB16 ? 2 : 1;
    memset(bytes, 0, sizeof bytes);
    for (unsigned i = 0; i < count; ++i) {
        if (silent_wave)
            break;
        unsigned intensity = 12 + current_track % 6 * 9 + (i + first) / 1250;
        uint8_t *column = bytes + i * stride;
        if (stride == 3) {
            column[0] = (uint8_t)(intensity + ((i + first) % 30 == 0 ? 12 : 0));
            column[1] = (uint8_t)intensity;
            column[2] = (uint8_t)(intensity / 2);
        } else if (stride == 2) {
            column[1] = (uint8_t)((intensity / 4) << 2);
        } else {
            column[0] = (uint8_t)(intensity / 4);
        }
    }
    static az_wave_snapshot wave;
    wave = (az_wave_snapshot){.encoding = wave_format, .first_column = first,
        .column_count = count, .total_columns = total, .bytes_per_column = wave_invalid ? 0 : stride,
        .data_bytes = unfinished_wave ? 0 : count * stride, .seconds_per_column = 1.0 / 150,
        .columns = bytes};
    *out = &wave;
    return AZ_OK;
}

static int move(const char *id, const az_playlist_move_v1 *request, uint64_t *number)
{
    assert(strcmp(id, SP_ID) == 0 && request->revision == revision && request->stick == 0);
    assert(request->from <= length && request->to <= length && request->from != request->to);
    assert(request->entry_id == entries[request->from - 1].entry_id);
    assert(request->source_generation == generation && request->library_id == 9);
    saved = *request;
    *number = ++receipt;
    pending = 1;
    return AZ_PENDING;
}

static int move_status(const char *id, uint64_t number, az_playlist_move_result_v1 *result)
{
    assert(strcmp(id, SP_ID) == 0 && number == receipt);
    if (pending) {
        --pending;
        return AZ_PENDING;
    }
    int from = (int)saved.from - 1, to = (int)saved.to - 1;
    az_playlist_entry_v1 row = entries[from];
    if (from < to)
        memmove(entries + from, entries + from + 1, (size_t)(to - from) * sizeof row);
    else
        memmove(entries + to + 1, entries + to, (size_t)(from - to) * sizeof row);
    entries[to] = row;
    ++moves;
    ++revision;
    busy_order = true;
    *result = (az_playlist_move_result_v1){.size = sizeof *result, .version = AZ_DATA_VERSION,
        .status = AZ_OK, .outcome = AZ_PLAYLIST_MOVED,
        .device_library = moves == fail_sync ? AZ_PDB_FAILED : AZ_PDB_CURRENT};
    return AZ_OK;
}

static az_library_v1 library = {.abi = AZ_LIBRARY_ABI, .size = sizeof library,
    .lib_playlists = playlists, .lib_tracks = tracks, .track_resolve = resolve,
    .track_request = request, .track_acquire = acquire, .track_release = release,
    .waveform_request = wave_request, .waveform_acquire = wave_acquire,
    .playlist_order = order, .playlist_move = move, .playlist_move_status = move_status};

static const az_library_v1 *get_library(uint32_t abi)
{
    assert(abi == AZ_LIBRARY_ABI);
    return &library;
}

static const char *data_dir(const char *id)
{
    assert(strcmp(id, SP_ID) == 0);
    return have_data ? data : NULL;
}

static int register_screen(const char *id, const az_screen_v1 *screen)
{
    assert(strcmp(id, SP_ID) == 0 && screen->flags == AZ_SCREEN_DEDICATED);
    registered = screen;
    return 0;
}

static const int surface_handle = 0;

static az_surface *make_surface(const char *id, const az_surface_v1 *desc)
{
    assert(strcmp(id, SP_ID) == 0 && desc->views == AZ_VIEW_BIT(AZ_VIEW_BROWSE));
    planner = desc;
    running = desc->user;
    return (az_surface *)&surface_handle;
}

static void place_surface(az_surface *s, az_rect area)
{
    assert(s == (const void *)&surface_handle);
    planner_area = area;
}

static void show_surface(az_surface *s, int show)
{
    assert(s == (const void *)&surface_handle);
    planner_visible = show;
    if (!show)
        planner_focus = false;
}

static int focus_surface(az_surface *s, int focus)
{
    assert(s == (const void *)&surface_handle && planner_visible);
    planner_focus = focus && !focus_refused;
    return planner_focus;
}

static void redraw_surface(az_surface *s)
{
    assert(s == (const void *)&surface_handle);
    ++redraws;
}

static int view(void) { return current_view; }

static int register_plan(const char *id, az_plan_set_fn fn, void *user)
{
    assert(strcmp(id, SP_ID) == 0 && !menu_action);
    menu_action = fn;
    menu_user = user;
    return AZ_OK;
}

static int resolve_target(const char *id, const az_plan_set_target_v1 *target,
                          az_plan_set_source_v1 *out)
{
    assert(strcmp(id, SP_ID) == 0 && out->size == sizeof *out);
    if (strcmp(drive, target->mount) != 0)
        return AZ_STALE;
    out->stick = 0;
    out->source_generation = generation + stale_target;
    out->library_id = 9;
    return AZ_OK;
}

static int cfg_get(const char *id, const char *key, int value)
{
    assert(strcmp(id, SP_ID) == 0 && key);
    return value;
}

static void cfg_set(const char *id, const char *key, int value)
{
    assert(strcmp(id, SP_ID) == 0 && key && value >= 0);
    ++cfg_changes;
}

static int thread_start(const char *id, const char *name, void *(*fn)(void *), void *arg)
{
    assert(strcmp(id, SP_ID) == 0 && strcmp(name, "planner") == 0);
    return pthread_create(&worker, NULL, fn, arg);
}

static int confirm(const char *id, const char *title, const char *body, const char *yes,
                   void (*fn)(int, void *), void *user)
{
    assert(strcmp(id, SP_ID) == 0 && title && body && yes && !confirmation);
    confirmation = fn;
    confirmation_user = user;
    return 0;
}

static void respond(int yes)
{
    assert(confirmation);
    void (*answer)(int, void *) = confirmation;
    confirmation = NULL;
    answer(yes, confirmation_user);
}

static void draw_text(az_draw *draw, int x, int y, const char *text, int px, uint32_t rgb)
{
    (void)draw; (void)x; (void)y; (void)px; (void)rgb;
    size_t len = strlen(drawn);
    snprintf(drawn + len, sizeof drawn - len, "%s\n", text);
}

static void fill_rect(az_draw *draw, int x, int y, int w, int h, uint32_t rgb)
{
    (void)draw; (void)x; (void)y; (void)rgb;
    assert(w > 0 && h > 0);
}

static uint32_t color(int role) { return (uint32_t)role; }
static void redraw(void) { ++redraws; }
static int ui_theme(void) { return planner_visible ? 0 : 2; }
static int text_width(const char *label, int px) { return (int)strlen(label) * px / 2; }
static int row_height(az_draw *draw) { (void)draw; return 48; }

static const az_core_v1 core = {.size = sizeof core, .library_service = get_library,
    .data_dir = data_dir, .register_screen = register_screen, .thread_start = thread_start,
    .confirm = confirm, .draw_text = draw_text, .fill_rect = fill_rect,
    .color = color, .request_redraw = redraw, .cfg_get = cfg_get, .cfg_set = cfg_set,
    .text_width = text_width, .row_height = row_height, .ui_theme = ui_theme,
    .surface_create = make_surface, .surface_place = place_surface, .surface_show = show_surface,
    .surface_focus = focus_surface, .surface_redraw = redraw_surface, .view = view,
    .on_plan_set = register_plan, .plan_set_resolve = resolve_target};

static SpRuntime direct_runtime(void)
{
    return (SpRuntime){.core = &core, .library = &library,
        .model = {.stick = 0, .playlist = 44},
        .options = {.opener = -1, .closer = -1, .max_bpm = 18, .curve = SP_PRIMETIME,
            .time_aware = true, .halftime = true},
        .energy_choice = {SP_PHRASES, AZ_PHRASE_CHORUS, SP_FIRST}};
}

static void test_library(void)
{
    SpRuntime r = direct_runtime();
    fixture(LARGE);
    assert(sp_read_selection(&r) == AZ_OK && r.model.source.rows[LARGE - 1].position == LARGE);
    changed_page = true;
    assert(sp_read_selection(&r) == AZ_STALE);
    changed_page = false;
    bad_row = true;
    assert(sp_read_selection(&r) == AZ_STALE);
    bad_row = false;
    bad_tracks = true;
    assert(sp_read_selection(&r) == AZ_NOT_FOUND);
    bad_tracks = false;
    roster_mismatch = true;
    assert(sp_read_selection(&r) == AZ_STALE);
    fixture(0);
    assert(sp_read_selection(&r) == AZ_EMPTY);
    length = SP_MAX_ENTRIES + 1;
    assert(sp_read_selection(&r) == AZ_LIMIT);
    fixture(SMALL);
    assert(sp_read_selection(&r) == AZ_OK);
    ++revision;
    assert(sp_order_check(&r) == AZ_STALE);
    assert(sp_create_preview(&r) == AZ_STALE);
    r.launch = (az_plan_set_target_v1){.size = sizeof r.launch, .version = AZ_DATA_VERSION,
        .playlist_id = 44};
    assert(sp_load_target(&r) == AZ_STALE);
    assert(strlen(drive) < sizeof r.launch.mount);
    memcpy(r.launch.mount, drive, strlen(drive) + 1);
    assert(sp_load_target(&r) == AZ_OK && strcmp(r.model.playlist_name, "Party") == 0);
    stale_target = true;
    assert(sp_load_target(&r) == AZ_STALE && !r.model.tracks);
    stale_target = false;
    sp_release_model(&r.model);
}

static void test_energy(void)
{
    SpRuntime r = direct_runtime();
    fixture(SMALL);
    assert(sp_read_selection(&r) == AZ_OK);
    busy_release = true;
    sp_analyze_track(&r, 0);
    assert(r.model.inputs[0].energy == SP_PHRASES && requests == 1 && waves == 4);
    assert(wave_first[0] == 75 && wave_first[1] == 2625 && wave_first[2] == 4500 &&
           wave_first[3] == 9000);
    assert(wave_columns[0] == 1050 && wave_columns[1] == 825);
    assert(r.model.inputs[0].score > 0.4 && r.model.inputs[0].score < 0.7);
    r.energy_choice.mode = SP_PHRASE;
    r.energy_choice.occurrence = SP_LAST;
    sp_analyze_track(&r, 1);
    assert(r.model.inputs[1].energy == SP_PHRASE && waves == 5 && wave_first[4] == 9000);
    r.energy_choice.occurrence = SP_MIDDLE;
    sp_analyze_track(&r, 2);
    assert(waves == 6 && wave_first[5] == 2625);
    r.energy_choice.occurrence = SP_FIRST;
    sp_analyze_track(&r, 3);
    assert(waves == 7 && wave_first[6] == 75);
    r.energy_choice.mode = SP_FULL;
    unsigned before = requests;
    sp_analyze_track(&r, 4);
    assert(r.model.inputs[4].energy == SP_FULL && requests == before && waves == 10);
    assert(wave_first[7] == 0 && wave_first[8] == 4096 && wave_first[9] == 8192);
    wave_invalid = true;
    sp_analyze_track(&r, 5);
    assert(r.model.inputs[5].energy == SP_MISSING);
    wave_invalid = false;
    unfinished_wave = true;
    sp_analyze_track(&r, 5);
    assert(r.model.inputs[5].energy == SP_MISSING);
    unfinished_wave = false;
    r.energy_choice.mode = SP_PHRASES;
    r.energy_choice.kind = AZ_PHRASE_BRIDGE;
    sp_analyze_track(&r, 5);
    assert(r.model.inputs[5].energy == SP_FULL && r.model.inputs[5].score > 0);
    no_phrases = true;
    sp_analyze_track(&r, 5);
    assert(r.model.inputs[5].energy == SP_FULL);
    no_phrases = false;
    r.energy_choice.kind = AZ_PHRASE_CHORUS;
    short_map = true;
    sp_analyze_track(&r, 5);
    assert(r.model.inputs[5].energy == SP_FULL);
    short_map = false;
    wave_format = AZ_WAVE_PWV5_RGB16;
    sp_analyze_track(&r, 5);
    assert(r.model.inputs[5].energy == SP_PHRASES && r.model.inputs[5].score > 0 &&
        r.model.inputs[5].encoding == AZ_WAVE_PWV5_RGB16);
    wave_format = AZ_WAVE_PWV3_MONO;
    sp_analyze_track(&r, 5);
    assert(r.model.inputs[5].energy == SP_PHRASES && r.model.inputs[5].score > 0 &&
        r.model.inputs[5].encoding == AZ_WAVE_PWV3_MONO);
    silent_wave = true;
    sp_analyze_track(&r, 5);
    assert(r.model.inputs[5].energy == SP_MISSING);
    sp_release_model(&r.model);
}

static int truncate_records(const char *path, const struct stat *info, int type, struct FTW *location)
{
    (void)info; (void)location;
    if (type == FTW_F)
        assert(truncate(path, 1) == 0);
    return 0;
}

static void test_cache(void)
{
    SpRuntime r = direct_runtime();
    fixture(SMALL);
    assert(sp_read_selection(&r) == AZ_OK && !sp_cache_read(&r, 0));
    r.job = SP_PREVIEW;
    assert(sp_create_preview(&r) == AZ_OK && requests == 6 && r.model.cache_hits == 1);
    double chorus = r.model.inputs[0].score;
    unsigned before = waves;
    sp_release_model(&r.model);
    ++generation;
    assert(sp_read_selection(&r) == AZ_OK);
    assert(sp_create_preview(&r) == AZ_OK && waves == before && r.model.cache_hits == SMALL);
    assert(r.model.inputs[0].score == chorus);
    r.model.playlist = 45;
    assert(sp_read_selection(&r) == AZ_OK);
    assert(sp_create_preview(&r) == AZ_OK && waves == before && r.model.cache_hits == SMALL);
    r.energy_choice.mode = SP_FULL;
    assert(sp_create_preview(&r) == AZ_OK && waves == before + 18 && r.model.cache_hits == 1);
    r.energy_choice.mode = SP_PHRASE;
    assert(sp_create_preview(&r) == AZ_OK && waves == before + 24 && r.model.cache_hits == 1);
    r.energy_choice.mode = SP_PHRASES;
    assert(sp_create_preview(&r) == AZ_OK && waves == before + 24 && r.model.cache_hits == SMALL);
    test_bpm = 116;
    assert(sp_create_preview(&r) == AZ_OK && r.model.cache_hits == SMALL - 1);
    test_bpm = 0;
    r.job = SP_REFRESH;
    assert(sp_create_preview(&r) == AZ_OK && r.model.cache_hits == 0);
    r.energy_choice.mode = SP_FULL;
    assert(!sp_cache_read(&r, 0));
    r.energy_choice.mode = SP_PHRASES;
    assert(sp_cache_read(&r, 0));
    have_data = false;
    assert(!sp_cache_read(&r, 0));
    have_data = true;
    snprintf(r.model.inputs[0].path, sizeof r.model.inputs[0].path, "/another-usb/a.aiff");
    assert(!sp_cache_read(&r, 0));
    wave_missing = true;
    assert(sp_create_preview(&r) == AZ_OK && r.model.unknown_energy == SMALL);
    assert(!sp_cache_read(&r, 0));
    r.job = SP_PREVIEW;
    wave_missing = false;
    assert(sp_create_preview(&r) == AZ_OK && r.model.cache_hits == 1);
    running = &r;
    cancel_wave = true;
    r.job = SP_REFRESH;
    assert(sp_create_preview(&r) == AZ_CANCELLED && !r.model.target);
    assert(sp_cache_read(&r, 0));
    assert(nftw(data, truncate_records, 8, FTW_PHYS) == 0);
    assert(!sp_cache_read(&r, 0));
    cancel_wave = false;
    atomic_store(&r.cancel, false);
    r.job = SP_PREVIEW;
    assert(sp_create_preview(&r) == AZ_OK && r.model.cache_hits == 1);
    r.energy_choice.kind = AZ_PHRASE_BRIDGE;
    wave_format = AZ_WAVE_PWV3_MONO;
    assert(sp_create_preview(&r) == AZ_OK && r.model.fallback_energy == SMALL &&
        r.model.cache_hits == 1);
    before = waves;
    assert(sp_create_preview(&r) == AZ_OK && r.model.cache_hits == SMALL && waves == before);
    assert(r.model.inputs[0].energy == SP_FULL && r.model.inputs[0].encoding == AZ_WAVE_PWV3_MONO);
    sp_release_model(&r.model);
}

static void assert_permutation(const int *indices, int count)
{
    bool seen[LARGE] = {0};
    for (int i = 0; i < count; ++i) {
        assert(indices[i] >= 0 && indices[i] < count && !seen[indices[i]]);
        seen[indices[i]] = true;
    }
}

static void check_moves(int *target, int count)
{
    int order[8], longest = 0, length_from[8];
    SpMove plan[8];
    SpOptions options = {0};
    for (int i = 0; i < count; ++i) {
        order[i] = i;
        length_from[i] = 1;
        for (int j = 0; j < i; ++j)
            if (target[i] > target[j] && length_from[i] <= length_from[j])
                length_from[i] = length_from[j] + 1;
        if (length_from[i] > longest)
            longest = length_from[i];
    }
    int moves = sp_plan_moves(target, count, plan, &options);
    assert(moves == count - longest);
    for (int i = 0; i < moves; ++i) {
        assert(order[plan[i].from] == plan[i].entry);
        sp_relocate(order, plan[i].from, plan[i].to);
    }
    assert(memcmp(order, target, (size_t)count * sizeof *order) == 0);
}

static void permute(int *tracks, int count, int next)
{
    if (next == count) {
        check_moves(tracks, count);
        return;
    }
    for (int i = next; i < count; ++i) {
        int value = tracks[i];
        tracks[i] = tracks[next];
        tracks[next] = value;
        permute(tracks, count, next + 1);
        tracks[next] = tracks[i];
        tracks[i] = value;
    }
}

static void test_algorithm(void)
{
    int indices[] = {0, 1, 2, 3, 4, 5, 6, 7};
    permute(indices, 8, 0);
    SpTrack tracks[LARGE] = {0};
    int planned[LARGE];
    SpOptions options = {.opener = 6, .closer = 1, .max_bpm = 18, .curve = SP_PRIMETIME,
        .time_aware = true, .halftime = true};
    static const char *keys[] = {"  8A", "12B", "B♭ minor", "F♯", "G#M", "Abmaj",
        "c#min", " A major", "Dm", "C weird", "?", "6x", "8AA", NULL};
    for (int i = 0; i < LARGE; ++i) {
        tracks[i] = (SpTrack){.bpm = i % 11 ? 80 + i % 4 * 10 + i % 7 : 0,
            .energy = (i % 12) / 12.0, .energy_known = i % 4 != 0, .duration = 200};
        sp_track_key(&tracks[i], keys[i % (sizeof keys / sizeof *keys)]);
    }
    assert(tracks[0].key == 8 && !tracks[0].major && tracks[1].key == 12 && tracks[1].major);
    assert(tracks[2].key == 3 && !tracks[2].major && tracks[9].key == 0);
    assert(sp_normalize_energy(tracks, LARGE));
    assert(tracks[12].energy == 5 && tracks[1].energy >= 1 && tracks[11].energy == 10);
    for (int n = 12; n < LARGE; n += 40) {
        assert(sp_plan_order(tracks, n, &options, planned));
        assert_permutation(planned, n);
        assert(planned[0] == 6 && planned[n - 1] == 1);
        assert(sp_measure(tracks, n, planned, &options).score <=
               sp_measure(tracks, n, NULL, &options).score + 10000);
    }
    options.opener = -1;
    options.closer = -1;
    options.halftime = false;
    options.time_aware = false;
    for (int n = 1; n < 21; ++n) {
        assert(sp_plan_order(tracks, n, &options, planned));
        assert_permutation(planned, n);
    }
    SpTrack equal[3] = {{.energy = 0.5, .energy_known = true},
        {.energy = 0.5, .energy_known = true}, {.energy = 0.5, .energy_known = true}};
    assert(sp_normalize_energy(equal, 3) && equal[0].energy == 5);
    assert(sp_plan_order(tracks, 1, &options, planned) && planned[0] == 0);
}

static void screen_draw(const char *expected)
{
    drawn[0] = '\0';
    if (planner_visible)
        planner->on_paint(NULL, planner_area, planner->user);
    else
        registered->on_draw(NULL, (az_rect){0, 0, 1280, 650}, registered->user);
    if (!strstr(drawn, expected))
        fprintf(stderr, "Expected screen text %s. Screen was\n%s", expected, drawn);
    assert(strstr(drawn, expected));
}

static int input_at(int kind, int direction, int x, int y)
{
    az_event event = {.kind = kind, .dir = direction, .x = x, .y = y};
    if (planner_visible) {
        event.y += planner_area.y;
        return planner->on_input(&event, planner->user);
    }
    return registered->on_input(&event, registered->user);
}

static int input(int kind, int direction, int y)
{
    return input_at(kind, direction, 30, y);
}

static void click_row(int index)
{
    enum SpPage page = running->page;
    if (page == SP_ORDER && index == 0) {
        assert(input_at(AZ_EV_TAP, 0, 1100, 22));
        return;
    }
    int top = page == SP_PICK_START || page == SP_PICK_END ? 92 : 56;
    assert(input(AZ_EV_TAP, 0, top + index * 48));
}

static void finish(const az_mod_v1 *mod, int expected)
{
    for (int i = 0; i < 400 && atomic_load(&running->phase) != SP_DONE; ++i) {
        mod->tick();
        sp_delay();
    }
    assert(atomic_load(&running->phase) == SP_DONE && running->result == expected);
    mod->tick();
    assert(atomic_load(&running->phase) == SP_IDLE);
}

static void notify_action(uint32_t id, bool valid, const char *name)
{
    az_plan_set_target_v1 target = {.size = sizeof target, .version = AZ_DATA_VERSION,
        .playlist_id = id};
    if (valid) {
        assert(strlen(drive) < sizeof target.mount);
        memcpy(target.mount, drive, strlen(drive) + 1);
    }
    snprintf(target.name, sizeof target.name, "%s", name ? name : "");
    menu_action(&target, menu_user);
}

static void test_defaults(void)
{
    screen_draw("Set Planner settings");
    assert(!strstr(drawn, "Preview set"));
    click_row(0);
    screen_draw("All of a type");
    click_row(0);
    screen_draw("Full track");
    click_row(0);
    screen_draw("Occurrence");
    click_row(1);
    click_row(2);
    screen_draw("Middle");
    click_row(0);
    for (int i = 0; i < 6; ++i)
        click_row(1);
    assert(input(AZ_EV_BACK, 0, 0));
    click_row(1);
    click_row(2);
    click_row(3);
    click_row(4);
    screen_draw("Closing");
    assert(!input(AZ_EV_BACK, 0, 0) && cfg_changes == 15);
}

static void pin_tracks(void)
{
    screen_draw("Automatic");
    click_row(0);
    screen_draw("Choose opener");
    click_row(3);
    click_row(1);
    screen_draw("Choose closer");
    click_row(1);
    assert(running->options.opener == 2 && running->options.closer == 0);
}

static void preview_playlist(const az_mod_v1 *mod, uint32_t id, const char *name)
{
    notify_action(id, true, name);
    finish(mod, AZ_OK);
    screen_draw(name);
    click_row(2);
    finish(mod, AZ_OK);
}

static void test_display_energy(const az_mod_v1 *mod)
{
    fixture(SMALL);
    preview_playlist(mod, 44, "Party");
    double relative = running->model.tracks[0].energy;
    double raw = running->model.inputs[0].score;
    char display[24];
    snprintf(display, sizeof display, "\n%ld\n", lround(raw * 100));
    screen_draw(display);
    assert(input(AZ_EV_BACK, 0, 0) && input(AZ_EV_BACK, 0, 0));

    --length;
    ++revision;
    preview_playlist(mod, 45, "After Party");
    assert(running->model.tracks[0].energy != relative && running->model.inputs[0].score == raw);
    screen_draw(display);
    SpInput *track = &running->model.inputs[0];
    track->encoding = AZ_WAVE_PWV5_RGB16;
    track->score = 0.624;
    screen_draw("\n62*\n");
    track->score = 0.625;
    screen_draw("\n63*\n");
    track->score = 0.0001;
    screen_draw("\n0*\n");
    track->score = 0.999;
    screen_draw("\n100*\n");
    track->energy = SP_MISSING;
    screen_draw("\n-\n");
    assert(input(AZ_EV_BACK, 0, 0) && input(AZ_EV_BACK, 0, 0));
}

static void test_transition_display(const az_mod_v1 *mod)
{
    fixture(SMALL);
    --length;
    preview_playlist(mod, 44, "Party");
    SpModel *model = &running->model;
    running->options.halftime = true;
    running->options.max_bpm = 8;
    static const double bpm[] = {100, 120, 123, 60, 140, 0};
    static const char *const key[] = {"8A", "9A", "2A", "8A", "2A", ""};
    for (uint32_t i = 0; i < length; ++i) {
        SpTrack *track = &model->tracks[model->target[i]];
        track->bpm = bpm[i];
        snprintf(track->key_text, sizeof track->key_text, "%s", key[i]);
        sp_track_key(track, key[i]);
    }
    SpMetrics metrics = sp_measure(model->tracks, (int)length, model->target, &running->options);
    assert(metrics.tempo_jumps == 2 && metrics.key_warnings == 3 && metrics.compatible == 1);
    assert(metrics.unknown_bpm == 1 && metrics.unknown_keys == 1);
    assert(metrics.max_bpm == 20 && metrics.max_raw_bpm == 80);
    screen_draw("\n100.0\n8A\n");
    screen_draw("\n120.0\nJump 20.0\n9A\n");
    screen_draw("\n123.0\n2A\nKey clash\n");
    screen_draw("\n60.0\n8A\nKey clash\n");
    screen_draw("\n140.0\nJump 20.0\n2A\nKey clash\n");
    screen_draw("\n-\n-\n");

    az_rect full_area = planner_area;
    planner_area.h = 240;
    running->cursor = running->top = 4;
    screen_draw("\n60.0\n8A\nKey clash\n");
    assert(!strstr(drawn, "\n123.0\n"));
    running->options.halftime = false;
    screen_draw("\n60.0\nJump 63.0\n8A\nKey clash\n");
    planner_area = full_area;
    assert(input(AZ_EV_BACK, 0, 0) && input(AZ_EV_BACK, 0, 0));
}

static void test_screen(void)
{
    fixture(SMALL);
    long_title = true;
    const az_mod_v1 *mod = az_mod_v1_entry();
    assert(mod->init(NULL) == AZ_UNSUPPORTED);
    az_core_v1 incomplete = core;
    incomplete.draw_text = NULL;
    assert(mod->init(&incomplete) == AZ_UNSUPPORTED);
    assert(mod->init(&core) == 0);
    test_defaults();
    notify_action(0, true, "Party");
    assert(!planner_visible);
    current_view = AZ_VIEW_SEARCH;
    notify_action(44, true, "Party");
    assert(!planner_visible);
    current_view = AZ_VIEW_BROWSE;
    focus_refused = true;
    notify_action(44, true, "Party");
    assert(!planner_visible);
    focus_refused = false;
    notify_action(44, false, NULL);
    assert(planner_visible && planner_focus);
    finish(mod, AZ_STALE);
    screen_draw("playlist changed");
    assert(input(AZ_EV_BACK, 0, 0) && !planner_visible && !planner_focus);
    notify_action(44, true, "Party");
    screen_draw("Reading tracks");
    finish(mod, AZ_OK);
    screen_draw("Plan set\nParty");
    assert(running->options.curve == SP_CLOSING && running->options.max_bpm == 4);
    assert(running->energy_choice.kind == AZ_PHRASE_CHORUS);
    assert(running->energy_choice.occurrence == SP_MIDDLE);
    pin_tracks();
    click_row(3);
    finish(mod, AZ_OK);
    screen_draw("Set preview");
    assert(strstr(drawn, "..."));
    assert(strstr(drawn, "Bb") && strstr(drawn, "F#"));
    assert(strstr(drawn, "Edi" "\xe7\xe3" "o"));
    assert(running->model.target[0] == 2 && running->model.target[SMALL - 1] == 0);
    assert(running->model.move_count >= 2);
    click_row(0);
    respond(0);
    assert(moves == 0 && atomic_load(&running->phase) == SP_IDLE);
    ++revision;
    click_row(0);
    respond(1);
    finish(mod, AZ_STALE);
    assert(moves == 0);
    screen_draw("playlist changed");
    assert(input(AZ_EV_BACK, 0, 0));
    assert(input(AZ_EV_BACK, 0, 0) && !planner_visible && !planner_focus);
    notify_action(44, true, NULL);
    finish(mod, AZ_OK);
    screen_draw("Plan set\nParty");
    pin_tracks();
    click_row(2);
    finish(mod, AZ_OK);
    screen_draw("7 cached");
    assert(strstr(drawn, "Energy\n") && !strstr(drawn, "\n0.0\n"));
    az_playlist_entry_v1 expected[SMALL];
    for (int i = 0; i < SMALL; ++i)
        expected[i] = entries[running->model.target[i]];
    fail_sync = 1;
    click_row(0);
    respond(1);
    finish(mod, AZ_FAILED);
    screen_draw("Device Library sync failed");
    assert(moves == 1 && running->model.verified == 1 && running->model.can_resume);
    fail_sync = 0;
    click_row(0);
    respond(1);
    finish(mod, AZ_OK);
    screen_draw("playlist moves verified");
    assert(!running->sync_failed && !running->model.can_resume && moves == (unsigned)running->model.move_count);
    for (int i = 0; i < SMALL; ++i)
        assert(entries[i].entry_id == expected[i].entry_id);
    assert(input(AZ_EV_BACK, 0, 0) && !planner_visible && !planner_focus);
    test_display_energy(mod);
    test_transition_display(mod);
    notify_action(44, true, "Party");
    finish(mod, AZ_OK);
    current_view = AZ_VIEW_SOURCE;
    mod->tick();
    assert(!planner_visible && !planner_focus && redraws > 0);
    atomic_store(&running->stop, true);
    assert(pthread_join(worker, NULL) == 0);
}

static int cleanup(const char *path, const struct stat *info, int type, struct FTW *location)
{
    (void)info; (void)type; (void)location;
    return remove(path);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    assert(tmp);
    char base[512];
    assert(snprintf(base, sizeof base, "%s/sp-tests.XXXXXX", tmp) > 0 && mkdtemp(base));
    snprintf(drive, sizeof drive, "%s/usb", base);
    char mods[512], community[512], plugin[512];
    snprintf(mods, sizeof mods, "%s/MODS", drive);
    snprintf(community, sizeof community, "%s/community-mods", mods);
    snprintf(plugin, sizeof plugin, "%s/%s", community, SP_ID);
    snprintf(data, sizeof data, "%s/data", plugin);
    assert(mkdir(drive, 0700) == 0 && mkdir(mods, 0700) == 0 && mkdir(community, 0700) == 0);
    assert(mkdir(plugin, 0700) == 0 && mkdir(data, 0700) == 0);
    test_algorithm();
    test_library();
    test_energy();
    test_cache();
    test_screen();
    assert(nftw(base, cleanup, 8, FTW_DEPTH | FTW_PHYS) == 0);
    puts("Set Planner tests passed");
}
