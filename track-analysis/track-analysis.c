/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "azmod-mod.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "analysis/analysis.h"
#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#endif

#define ID "track-analysis"
static const az_core_v1 *C;
static const az_library_v1 *L;
static int enabled_row;
static az_deck_subscription subscription;
static struct desired { az_track_ref track; uint64_t epoch, attempted; int loaded; } desired[4];
static struct work {
    atomic_int phase; /* 0 UI owns; 1 queued; 2 worker owns; 3 UI collects. */
    atomic_int cancelled;
    az_track_ref track;
    uint64_t epoch;
    int deck, result;
} work[4];
static char status_text[4][96];
static unsigned pending_links; /* worker-only; native spool waits for playback to stop */
static void pending_link(const az_analysis_job_v1 *input) {
    if (input->version == AZ_DATA_VERSION && input->size >= sizeof *input &&
        (input->flags & AZ_ANALYSIS_NEEDS_LINK) && input->track.source_id >= 1 && input->track.source_id <= 2)
        pending_links |= 1u << (input->track.source_id - 1);
}

static void pause_worker(void) {
    struct timespec delay = {0, 100000000};
    nanosleep(&delay, NULL);
}
static int retryable(int result) { return result == AZ_BUSY || result == AZ_LIMIT || result == AZ_STALE; }
static int field_ready(const az_status_topic_v1 *topic, unsigned field,
                       const az_status_field_info_v1 *info, uint64_t epoch) {
    return topic && topic->version == AZ_DATA_VERSION && (topic->valid & (UINT64_C(1) << field)) &&
           info->info.availability == AZ_AVAIL_READY &&
           (info->scope == AZ_STATUS_UNIT || (info->scope == AZ_STATUS_LOAD_SCOPE && info->scope_id == epoch));
}
static void deck_status(const az_deck_status_event_v1 *event, void *user) {
    (void)user;
    if (!event || event->size < sizeof *event || event->version != AZ_DATA_VERSION || event->deck >= 4 ||
        !event->view || event->view->size < sizeof *event->view || !event->view->load) return;
    const az_status_load_v1 *load = event->view->load;
    if (load->header.size < sizeof *load) return;
    unsigned deck = event->deck;
    struct desired *d = &desired[deck];
    if (d->epoch != event->load_epoch) {
        atomic_store(&work[deck].cancelled, 1);
        d->epoch = event->load_epoch; d->attempted = 0;
    }
    d->loaded = event->load_epoch &&
        field_ready(&load->header, AZ_STATUS_LOAD_STATE, &load->state_info, event->load_epoch) &&
        load->state_info.info.provenance == AZ_PROV_NATIVE &&
        load->state == AZ_STATUS_LOAD_LOADED;
    memset(&d->track, 0, sizeof d->track);
    if (d->loaded && load->track_info.scope == AZ_STATUS_LOAD_SCOPE &&
        field_ready(&load->header, AZ_STATUS_LOAD_TRACK, &load->track_info, event->load_epoch)) d->track = load->track;
    if (!d->loaded) { d->attempted = 0; atomic_store(&work[deck].cancelled, 1); }
}
static int close_job(uint64_t job) {
    int rc;
    do { rc = L->analysis_close(ID, job); if (rc == AZ_BUSY) pause_worker(); } while (rc == AZ_BUSY);
    return rc;
}
struct cancellation {
    struct work *job;
    struct timespec started;
};
static int analysis_cancelled(void *user) {
    struct cancellation *cancel = user;
    struct timespec now;
    if (atomic_load(&cancel->job->cancelled)) return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 1;
    return now.tv_sec - cancel->started.tv_sec >= 600;
}
/* Apply policy only to the SDK-created analysis thread. Never alter the native
 * UI/audio thread or the process-wide address-space limit. */
static int background_policy(void) {
#ifdef __linux__
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(1, &cpus); CPU_SET(2, &cpus); CPU_SET(3, &cpus);
    return sched_setaffinity(0, sizeof cpus, &cpus) == 0 &&
           setpriority(PRIO_PROCESS, 0, 19) == 0;
#else
    return 1;
#endif
}
static int analyze(struct work *job) {
    az_analysis_job_v1 input = {.size = sizeof input};
    int rc = L->analysis_open_loaded_v2(ID, job->deck, job->epoch, &input);
    static int last_open[4] = {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
    if (AZ_HAS(C, log) && last_open[job->deck] != rc) {
        C->log(ID, "deck %d load %llu analysis lease: %d", job->deck + 1, (unsigned long long) job->epoch, rc);
        last_open[job->deck] = rc;
    }
    if (rc == AZ_NOT_FOUND) pending_link(&input);
    if (rc) return rc;
    if (input.version != AZ_DATA_VERSION || input.size < sizeof input || !input.job ||
        !(input.needed & (AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS)) ||
        (input.needed & ~(AZ_DATA_GRID | AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS)) ||
        !(input.flags & AZ_ANALYSIS_LOADED) ||
        !memchr(input.audio_path, 0, sizeof input.audio_path) || !memchr(input.analysis_path, 0, sizeof input.analysis_path)) {
        if (input.job) close_job(input.job);
        return AZ_FAILED;
    }
    const char *assets = C->assets_dir(ID);
    struct cancellation cancel = {.job = job};
    if (!assets || clock_gettime(CLOCK_MONOTONIC, &cancel.started)) {
        close_job(input.job); return AZ_FAILED;
    }
    az_beat *beats = NULL;
    az_analysis_grid_v1 grid = {.size = sizeof grid};
    ta_result *result = NULL;
    if (input.needed & AZ_DATA_PHRASES) {
        rc = L->analysis_grid(ID, input.job, &grid, NULL, 0);
        if (rc != AZ_LIMIT || grid.version != AZ_DATA_VERSION || !grid.beat_count || grid.beat_count > 32768) {
            if (rc == AZ_LIMIT || rc == AZ_OK) rc = AZ_FAILED;
            goto done;
        }
        beats = calloc(grid.beat_count, sizeof *beats);
        if (!beats) { rc = AZ_LIMIT; goto done; }
        rc = L->analysis_grid(ID, input.job, &grid, beats, grid.beat_count);
        if (rc) goto done;
    }
    if (analysis_cancelled(&cancel)) { rc = AZ_STALE; goto done; }
    ta_request request = {.audio_path = input.audio_path, .assets_path = assets,
        .beats = beats, .beat_count = grid.beat_count, .needed = input.needed,
        .bpm = input.original_bpm, .cancelled = analysis_cancelled, .user = &cancel};
    char error[160];
    int status = ta_analyze(&request, &result, error, sizeof error);
    rc = status == TA_CANCELLED ? AZ_STALE : AZ_FAILED;
    if (status != TA_OK) {
        if (AZ_HAS(C, log)) C->log(ID, "deck %d analysis: %s", job->deck + 1, error);
        goto done;
    }
    do {
        if (analysis_cancelled(&cancel)) { rc = AZ_STALE; break; }
        rc = L->analysis_commit_result(ID, input.job, ta_result_view(result));
        if (rc == AZ_BUSY) pause_worker();
    } while (rc == AZ_BUSY);
done:
    ta_result_free(result);
    free(beats);
    if (AZ_HAS(C, log)) C->log(ID, "deck %d load %llu analysis result: %d", job->deck + 1, (unsigned long long) job->epoch, rc);
    if (rc == AZ_OK) pending_link(&input);
    if (rc != AZ_OK) close_job(input.job);
    return rc;
}
static void *worker(void *unused) {
    (void)unused;
    unsigned spool_round = 0;
    int ready = background_policy();
    if (!ready && AZ_HAS(C, log)) C->log(ID, "analysis thread scheduling failed");
    for (;;) {
        for (int deck = 0; deck < 4; deck++) {
            int phase = 1;
            if (!atomic_compare_exchange_strong(&work[deck].phase, &phase, 2)) continue;
            work[deck].result = atomic_load(&work[deck].cancelled) ? AZ_STALE : ready ? analyze(&work[deck]) : AZ_UNSUPPORTED;
            if (retryable(work[deck].result))
                for (int wait = 0; wait < 10 && !atomic_load(&work[deck].cancelled); wait++) pause_worker();
            atomic_store(&work[deck].phase, 3);
        }
        if (++spool_round >= 20) {
            spool_round = 0;
            for (int stick = 0; stick < 2; stick++)
                if ((pending_links & (1u << stick)) && C->lib_spool_kick(ID, stick) == 0)
                    pending_links &= ~(1u << stick);
        }
        pause_worker();
    }
    return NULL;
}
static void tick(void) {
    for (int deck = 0; deck < 4; deck++) {
        struct work *job = &work[deck]; struct desired *d = &desired[deck];
        int phase = atomic_load(&job->phase);
        if (phase == 3) {
            if (job->epoch == d->epoch) {
                const char *message = job->result == AZ_OK ? "Track analysis ready" :
                    job->result == AZ_NOT_FOUND ? "Phrases and vocals ready" :
                    job->result == AZ_UNSUPPORTED ? "Unsupported audio or media source" :
                    job->result == AZ_STALE ? "Source changed or analysis cancelled" : "Analysis failed; see mod log";
                snprintf(status_text[deck], sizeof status_text[deck], "Deck %d: %s", deck + 1, message);
                if (job->result == AZ_OK) C->banner(ID, status_text[deck], 3000);
                if (retryable(job->result) && d->loaded && !atomic_load(&job->cancelled)) d->attempted = 0;
            }
            atomic_store(&job->phase, 0); phase = 0;
        }
        if (!C->row_value(enabled_row)) { atomic_store(&job->cancelled, 1); d->attempted = 0; continue; }
        if (!phase && d->loaded && d->epoch && d->attempted != d->epoch) {
            job->track = d->track; job->epoch = d->epoch; job->deck = deck; d->attempted = d->epoch;
            atomic_store(&job->cancelled, 0);
            snprintf(status_text[deck], sizeof status_text[deck], "Deck %d: Checking phrases and vocals", deck + 1);
            atomic_store(&job->phase, 1);
        }
    }
}
static void draw(az_draw *draw, az_rect area, void *user) {
    (void)user;
    C->draw_text(draw, area.x + 16, area.y + 16, "Track Analysis", 26, C->color(AZ_COL_TEXT));
    for (int deck = 0; deck < 4; deck++)
        C->draw_text(draw, area.x + 16, area.y + 64 + deck * 38,
            status_text[deck][0] ? status_text[deck] : "Waiting for a USB or Beatport track", 20, C->color(AZ_COL_TEXT_DIM));
}
static int init(const az_core_v1 *core) {
    if (!core || core->abi != AZ_MOD_ABI || !AZ_HAS(core, library_service) || !AZ_HAS(core, thread_start) ||
        !AZ_HAS(core, register_row) || !AZ_HAS(core, register_screen) || !AZ_HAS(core, assets_dir) ||
        !AZ_HAS(core, banner) || !AZ_HAS(core, lib_spool_kick)) return -1;
    C = core; L = C->library_service(AZ_LIBRARY_ABI);
    if (!AZ_LIB_HAS(L, analysis_grid) || !AZ_LIB_HAS(L, analysis_commit_result) ||
        !AZ_LIB_HAS(L, analysis_open_loaded_v2) || !AZ_LIB_HAS(L, analysis_close) ||
        !AZ_LIB_HAS(L, on_deck_status) || !AZ_LIB_HAS(L, deck_status_release)) return -2;
    az_row_v1 row = {.size = sizeof row, .key = "enabled", .label = "Track Analysis", .kind = AZ_ROW_TOGGLE,
        .flags = AZ_ROW_DEFAULT_ON, .help = "Generate missing phrases and vocal intervals automatically for USB and Beatport tracks."};
    enabled_row = C->register_row(ID, &row);
    if (enabled_row < 0) return -3;
    static const az_screen_v1 screen = {.size = sizeof screen, .on_draw = draw};
    if (C->register_screen(ID, &screen)) return -4;
    az_deck_watch_v1 watch = {.size = sizeof watch, .version = AZ_DATA_VERSION,
        .deck_mask = AZ_STATUS_ALL_DECKS, .topics = AZ_STATUS_TOPIC(AZ_STATUS_LOAD) | AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES)};
    if (L->on_deck_status(ID, &watch, deck_status, NULL, &subscription)) return -5;
    return C->thread_start(ID, "track-analysis", worker, NULL) ? -6 : 0;
}
static const char *const firmware[] = {"ep147-9e9454e357389ece", NULL};
static const az_mod_v1 mod = {.abi = AZ_MOD_ABI, .size = sizeof mod, .id = ID, .fw_tags = firmware, .init = init, .tick = tick};
AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void) { return &mod; }
