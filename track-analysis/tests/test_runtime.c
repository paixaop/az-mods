/* SPDX-License-Identifier: GPL-3.0-only */
#include "../track-analysis.c"
#include <assert.h>
static int enabled = 1, banners;
static int row_value(int row) { assert(row == 1); return enabled; }
static void banner(const char *id, const char *text, int ms) {
    assert(!strcmp(id, ID) && text && ms > 0); banners++;
}
static const az_screen_v1 *registered_screen;
static int drawn_lines;
static int register_screen(const char *id, const az_screen_v1 *screen) {
    assert(!strcmp(id, ID)); registered_screen = screen; return 0;
}
static int register_row(const char *id, const az_row_v1 *row) {
    assert(!strcmp(id, ID) && row->kind == AZ_ROW_TOGGLE); return 1;
}
static int thread_start(const char *id, const char *name, void *(*fn)(void *), void *arg) {
    assert(!strcmp(id, ID) && name && fn == worker && !arg); return 0;
}
static int analyzed, committed, closed, failure_mode, grid_reads, busy_once;
static unsigned needed_components = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS;
static const char *directory(const char *id) { assert(!strcmp(id, ID)); return "/private/assets"; }
static int process(const char *id, int pid) { (void)id; (void)pid; assert(0); return -1; }
struct ta_result { az_analysis_result_v1 view; };
int ta_analyze(const ta_request *r, ta_result **out, char *error, size_t size) {
    assert(r && !strcmp(r->audio_path, "/usb/audio.wav") && r->needed == needed_components);
    assert(r->bpm == 120 && r->cancelled && !r->cancelled(r->user));
    assert(r->beat_count == ((needed_components & AZ_DATA_PHRASES) ? 2u : 0u));
    assert(!strcmp(r->assets_path, "/private/assets"));
    if (r->beat_count) assert(r->beats[1].beat_in_bar == 2);
    ++analyzed;
    if (failure_mode == 1) { snprintf(error, size, "deliberate decode failure"); return TA_FAILED; }
    if (failure_mode == 2) {
        struct cancellation *cancel = r->user;
        atomic_store(&cancel->job->cancelled, 1);
        assert(r->cancelled(r->user));
    }
    *out = calloc(1, sizeof **out); assert(*out);
    (*out)->view = (az_analysis_result_v1){.size = sizeof(az_analysis_result_v1), .version = AZ_DATA_VERSION};
    return TA_OK;
}
const az_analysis_result_v1 *ta_result_view(const ta_result *r) { return &r->view; }
void ta_result_free(ta_result *r) { free(r); }
static int analysis_grid(const char *id, uint64_t job, az_analysis_grid_v1 *out, az_beat *beats, uint32_t capacity) {
    assert(!strcmp(id, ID) && job == 7 && out->size == sizeof *out);
    if (failure_mode == 3) return AZ_STALE;
    *out = (az_analysis_grid_v1){.size = sizeof *out, .version = AZ_DATA_VERSION, .beat_count = 2};
    if (!beats) { assert(!capacity); return AZ_LIMIT; }
    assert(capacity == 2); ++grid_reads;
    beats[0] = (az_beat){.bpm = 120, .beat_in_bar = 1};
    beats[1] = (az_beat){.time_s = 0.5, .bpm = 120, .beat_in_bar = 2};
    return AZ_OK;
}
static int analysis_open_loaded(const char *id, int deck, uint64_t epoch, az_analysis_job_v1 *out) {
    assert(deck == 0 && epoch == 44);
    assert(!strcmp(id, ID) && out->size == sizeof *out);
    *out = (az_analysis_job_v1){.size = sizeof *out, .version = AZ_DATA_VERSION,
        .needed = needed_components, .flags = AZ_ANALYSIS_LOADED | AZ_ANALYSIS_STREAMING, .job = 7, .original_bpm = 120,
        .audio_path = "/usb/audio.wav", .analysis_path = "/usb/ANLZ0000.DAT"};
    if (needed_components == AZ_ANALYSIS_VOCALS) out->analysis_path[0] = 0;
    return AZ_OK;
}
static int analysis_commit_result(const char *id, uint64_t job, const az_analysis_result_v1 *r) {
    assert(!strcmp(id, ID) && job == 7 && r && r->size == sizeof *r && r->version == AZ_DATA_VERSION);
    if (busy_once) { busy_once = 0; return AZ_BUSY; }
    ++committed; return AZ_OK;
}
static int release(const char *id, uint64_t handle) { assert(!strcmp(id, ID) && handle == 7); ++closed; return AZ_OK; }
static int subscribe(const char *id, const az_deck_watch_v1 *watch, az_deck_status_fn fn, void *user,
                     az_deck_subscription *out) {
    assert(!strcmp(id, ID) && watch->deck_mask == 15 && fn == deck_status && !user);
    *out = 1; return AZ_OK;
}
static int missing_typed_api;
static const az_library_v1 *library(unsigned abi) {
    static az_library_v1 lib = {.abi = AZ_LIBRARY_ABI, .size = sizeof lib,
        .analysis_open_loaded_v2 = analysis_open_loaded, .analysis_grid = analysis_grid, .analysis_commit_result = analysis_commit_result, .analysis_close = release,
        .on_deck_status = subscribe, .deck_status_release = release};
    lib.analysis_commit_result = missing_typed_api ? NULL : analysis_commit_result;
    assert(abi == AZ_LIBRARY_ABI); return &lib;
}
static uint32_t color(int role) { (void)role; return 0xffffff; }
static void draw_text(az_draw *d, int x, int y, const char *text, int px, uint32_t rgb) {
    (void)d; (void)x; (void)y; (void)px; (void)rgb; assert(text && *text); ++drawn_lines;
}
int main(void) {
    az_core_v1 core = {.abi = AZ_MOD_ABI, .size = sizeof core, .row_value = row_value, .banner = banner,
        .library_service = library, .register_row = register_row, .register_screen = register_screen,
        .thread_start = thread_start,
        .data_dir = directory, .assets_dir = directory, .lib_spool_kick = process,
        .draw_text = draw_text, .color = color};
    missing_typed_api = 1;
    assert(init(&core) == -2);
    missing_typed_api = 0;
    assert(init(&core) == AZ_OK);
    /* The loader retains this descriptor; it must survive init's stack frame. */
    assert(registered_screen && registered_screen->size >= (int)sizeof *registered_screen);
    registered_screen->on_draw(NULL, (az_rect){0, 0, 640, 480}, registered_screen->user);
    assert(drawn_lines == 5);
    az_status_load_v1 load = {.header = {.size = sizeof load, .version = AZ_DATA_VERSION,
        .valid = (UINT64_C(1) << AZ_STATUS_LOAD_STATE) | (UINT64_C(1) << AZ_STATUS_LOAD_TRACK)},
        .state = AZ_STATUS_LOAD_LOADED, .track = {.size = sizeof(az_track_ref), .version = AZ_DATA_VERSION,
            .source_id = 1, .source_generation = 2, .library_id = 3, .track_id = 4},
        .state_info = {.info = {.availability = AZ_AVAIL_READY, .provenance = AZ_PROV_NATIVE}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = 42},
        .track_info = {.info = {.availability = AZ_AVAIL_READY}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = 42}};
    az_deck_status_view_v1 view = {.size = sizeof view, .version = AZ_DATA_VERSION, .load = &load};
    az_deck_status_event_v1 event = {.size = sizeof event, .version = AZ_DATA_VERSION, .deck = 0,
        .load_epoch = 42, .view = &view};
    deck_status(&event, NULL); tick();
    assert(atomic_load(&work[0].phase) == 1 && work[0].track.track_id == 4 && work[0].epoch == 42);
    deck_status(&event, NULL); tick();
    assert(atomic_load(&work[0].phase) == 1 && desired[0].attempted == 42);
    atomic_store(&work[0].phase, 2);
    event.load_epoch = 43; load.track_info.scope_id = load.state_info.scope_id = 43;
    load.track.track_id = 5; deck_status(&event, NULL);
    assert(atomic_load(&work[0].cancelled));
    work[0].result = AZ_OK; atomic_store(&work[0].phase, 3); tick();
    assert(!banners && atomic_load(&work[0].phase) == 1 && work[0].epoch == 43);
    enabled = 0; tick();
    assert(atomic_load(&work[0].cancelled) && !desired[0].attempted);
    work[0].result = AZ_STALE; atomic_store(&work[0].phase, 3); tick();
    enabled = 1; tick(); assert(atomic_load(&work[0].phase) == 1);
    atomic_store(&work[0].phase, 3); work[0].result = AZ_OK; tick();
    assert(banners == 1 && atomic_load(&work[0].phase) == 0);
    desired[0].attempted = 0; tick();
    work[0].result = AZ_STALE; atomic_store(&work[0].phase, 3); tick();
    assert(atomic_load(&work[0].phase) == 1); /* cue/file edits retry while the same load remains */
    atomic_store(&work[0].cancelled, 1);
    atomic_store(&work[0].phase, 3); tick();
    assert(atomic_load(&work[0].phase) == 0);
    /* An old state scope is rejected; a native-only Beatport load schedules. */
    event.load_epoch = 44; load.header.valid = UINT64_C(1) << AZ_STATUS_LOAD_STATE;
    deck_status(&event, NULL); tick(); assert(!desired[0].loaded && !atomic_load(&work[0].phase));
    load.header.valid |= UINT64_C(1) << AZ_STATUS_LOAD_TRACK;
    load.track_info.scope = AZ_STATUS_UNIT;
    load.state_info.scope_id = event.load_epoch;
    deck_status(&event, NULL); tick(); assert(desired[0].loaded && atomic_load(&work[0].phase) == 1 && !work[0].track.track_id);
    /* No process callbacks are installed. All analysis runs in this process. */
    struct work job = {.deck = 0, .epoch = 44};
    assert(analyze(&job) == AZ_OK && analyzed == 1 && committed == 1 && grid_reads == 1 && !closed);
    needed_components = AZ_ANALYSIS_VOCALS;
    assert(analyze(&job) == AZ_OK && analyzed == 2 && committed == 2 && grid_reads == 1);
    needed_components = AZ_DATA_PHRASES;
    busy_once = 1;
    assert(analyze(&job) == AZ_OK && analyzed == 3 && committed == 3 && grid_reads == 2);
    failure_mode = 1;
    assert(analyze(&job) == AZ_FAILED && closed == 1 && committed == 3);
    failure_mode = 2;
    assert(analyze(&job) == AZ_STALE && closed == 2 && committed == 3);
    atomic_store(&job.cancelled, 0); failure_mode = 3;
    assert(analyze(&job) == AZ_STALE && closed == 3 && committed == 3);
    struct cancellation cancel = {.job = &job};
    assert(!clock_gettime(CLOCK_MONOTONIC, &cancel.started));
    cancel.started.tv_sec -= 601;
    assert(analysis_cancelled(&cancel));
    assert(init(NULL) == -1);
    puts("Track Analysis typed publication, load cancellation, lease cleanup, retries and no-process runtime passed");
}
