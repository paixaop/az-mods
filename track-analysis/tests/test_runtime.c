/* SPDX-License-Identifier: GPL-3.0-only */
#define _DARWIN_C_SOURCE
#define ta_unpack_worker checked_unpack_worker
#include "../track-analysis.c"
#undef ta_unpack_worker
#include <assert.h>
int ta_unpack_worker(const char *, int);
int checked_unpack_worker(const char *path, int fd) {
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    return ta_unpack_worker(path, fd);
}
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
static char test_directory[] = "/tmp/track-analysis-runtime-XXXXXX";
static int launched, committed;
static int launch(const char *id, const char *path, const char *const argv[], const az_launch_opts *opts) {
    assert(!strcmp(id, ID) && strstr(path, "/track-analysis-worker"));
    assert(!strcmp(argv[0], "/usb/audio.wav") && !strcmp(argv[1], "/usb/ANLZ0000.DAT"));
    assert(!strcmp(argv[2], "120.00000000") && strstr(argv[3], "/result-7-"));
    assert(strstr(argv[4], "/NativeBeatModels") && !argv[5]);
    assert(opts->nice == 19 && opts->mem_mb == 512);
    FILE *file = fopen(argv[3], "wb"); assert(file);
    const char result[16] = "worker result";
    assert(fwrite(result, 1, sizeof result, file) == sizeof result && !fclose(file));
    ++launched; return 123;
}
static int process(const char *id, int pid) { (void)id; (void)pid; assert(0); return -1; }
static int launch_alive(const char *id, int pid) { assert(!strcmp(id, ID) && pid == 123); return 0; }
static const char *directory(const char *id) { assert(!strcmp(id, ID)); return test_directory; }
static int analysis_open_loaded(const char *id, int deck, uint64_t epoch, az_analysis_job_v1 *out) {
    assert(deck == 0 && epoch == 44);
    assert(!strcmp(id, ID) && out->size == sizeof *out);
    *out = (az_analysis_job_v1){.size = sizeof *out, .version = AZ_DATA_VERSION,
        .needed = AZ_DATA_PHRASES, .flags = AZ_ANALYSIS_LOADED | AZ_ANALYSIS_STREAMING, .job = 7, .original_bpm = 120,
        .audio_path = "/usb/audio.wav", .analysis_path = "/usb/ANLZ0000.DAT"};
    return AZ_OK;
}
static int analysis_commit(const char *id, uint64_t job, const void *data, uint32_t size) {
    assert(!strcmp(id, ID) && job == 7 && size == 16 && !strcmp(data, "worker result"));
    ++committed; return AZ_OK;
}
static int release(const char *id, uint64_t handle) { (void)id; (void)handle; assert(0); return AZ_FAILED; }
static int subscribe(const char *id, const az_deck_watch_v1 *watch, az_deck_status_fn fn, void *user,
                     az_deck_subscription *out) {
    assert(!strcmp(id, ID) && watch->deck_mask == 15 && fn == deck_status && !user);
    *out = 1; return AZ_OK;
}
static const az_library_v1 *library(unsigned abi) {
    static const az_library_v1 lib = {.abi = AZ_LIBRARY_ABI, .size = sizeof lib,
        .analysis_open_loaded = analysis_open_loaded, .analysis_commit = analysis_commit, .analysis_close = release,
        .on_deck_status = subscribe, .deck_status_release = release};
    assert(abi == AZ_LIBRARY_ABI); return &lib;
}
static uint32_t color(int role) { (void)role; return 0xffffff; }
static void draw_text(az_draw *d, int x, int y, const char *text, int px, uint32_t rgb) {
    (void)d; (void)x; (void)y; (void)px; (void)rgb; assert(text && *text); ++drawn_lines;
}
int main(void) {
    az_core_v1 core = {.abi = AZ_MOD_ABI, .size = sizeof core, .row_value = row_value, .banner = banner,
        .library_service = library, .register_row = register_row, .register_screen = register_screen,
        .thread_start = thread_start, .launch = launch, .launch_alive = launch_alive, .launch_stop = process,
        .data_dir = directory, .assets_dir = directory, .lib_spool_kick = process,
        .draw_text = draw_text, .color = color};
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
    /* Exercise the real extraction/launch/result path, including the SDK argv contract. */
    assert(mkdtemp(test_directory));
    const unsigned char packed[] = {0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0xff,
        0x2b,0xcf,0x2f,0xca,0x4e,0x2d,0x52,0x48,0xcb,0xac,0x28,0x29,0x2d,0x4a,0xe5,
        0x02,0x00,0xfe,0xf2,0x23,0x52,0x0f,0x00,0x00,0x00};
    char packed_path[256], program_path[256];
    snprintf(packed_path, sizeof packed_path, "%s/track-analysis-worker.gz", test_directory);
    snprintf(program_path, sizeof program_path, "%s/track-analysis-worker", test_directory);
    FILE *file = fopen(packed_path, "wb"); assert(file);
    assert(fwrite(packed, 1, sizeof packed, file) == sizeof packed && !fclose(file));
    struct work job = {.deck = 0, .epoch = 44};
    assert(analyze(&job) == AZ_OK && launched == 1 && committed == 1);
    assert(!unlink(packed_path) && !unlink(program_path) && !rmdir(test_directory));
    assert(init(NULL) == -1);
    puts("Track Analysis screen lifetime, load cancellation, retries and worker launch tests passed");
}
