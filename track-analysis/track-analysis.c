/* SPDX-License-Identifier: GPL-3.0-only */
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
#include "inflate.h"

#define ID "track-analysis"
#define RESULT_MAX (16u + 24u + 32768u * 8u + 32u + 1024u * 24u)
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
    int result;
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
        load->state == AZ_STATUS_LOAD_LOADED &&
        load->track_info.scope == AZ_STATUS_LOAD_SCOPE &&
        field_ready(&load->header, AZ_STATUS_LOAD_TRACK, &load->track_info, event->load_epoch);
    if (d->loaded) d->track = load->track;
    else { d->attempted = 0; atomic_store(&work[deck].cancelled, 1); }
}
static int close_job(uint64_t job) {
    int rc;
    do { rc = L->analysis_close(ID, job); if (rc == AZ_BUSY) pause_worker(); } while (rc == AZ_BUSY);
    return rc;
}
/* Keep the package below the installer's 16 MiB ceiling. Inflate only on the
 * worker thread, into a private sibling; failed extraction never launches. */
static int prepare_program(const char *assets, const char *data, char program[1200]) {
    char packed[1200], staging[1240];
    if (snprintf(program, 1200, "%s/track-analysis-worker", data) >= 1200 ||
        snprintf(packed, sizeof packed, "%s/track-analysis-worker.gz", assets) >= (int)sizeof packed ||
        snprintf(staging, sizeof staging, "%s.tmp-XXXXXX", program) >= (int)sizeof staging) return 0;
    int fd = mkstemp(staging), ok = fd >= 0;
    if (ok) ok = ta_unpack_worker(packed, fd);
    if (fd >= 0) {
        if (ok && fchmod(fd, 0700)) {
            /* FAT mount masks can supply executable mode without accepting chmod. */
            struct stat executable;
            if (fstat(fd, &executable) || !(executable.st_mode & S_IXUSR)) ok = 0;
        }
        if (ok && fsync(fd)) ok = 0;
        if (close(fd)) ok = 0;
    }
    if (ok && rename(staging, program)) ok = 0;
    if (!ok && fd >= 0) unlink(staging);
    return ok;
}
static int analyze(struct work *job) {
    az_analysis_job_v1 input = {.size = sizeof input};
    int rc = L->analysis_open(ID, &job->track, &input);
    if (rc == AZ_NOT_FOUND) pending_link(&input);
    if (rc) return rc;
    if (input.version != AZ_DATA_VERSION || input.size < sizeof input || !input.job ||
        !(input.needed & AZ_DATA_PHRASES) || (input.needed & ~(AZ_DATA_GRID | AZ_DATA_PHRASES)) ||
        input.track.source_id != job->track.source_id || input.track.source_generation != job->track.source_generation ||
        input.track.library_id != job->track.library_id || input.track.track_id != job->track.track_id ||
        !memchr(input.audio_path, 0, sizeof input.audio_path) || !memchr(input.analysis_path, 0, sizeof input.analysis_path)) {
        if (input.job) close_job(input.job);
        return AZ_FAILED;
    }
    char program[1200], output[1200], log[1200], models[1200], bpm[48];
    const char *assets = C->assets_dir(ID), *data = C->data_dir(ID);
    if (!assets || !data ||
        snprintf(models, sizeof models, "%s/NativeBeatModels", assets) >= (int)sizeof models ||
        snprintf(output, sizeof output, "%s/result-%llu-XXXXXX", data, (unsigned long long)input.job) >= (int)sizeof output ||
        snprintf(log, sizeof log, "%s/analysis.log", data) >= (int)sizeof log) {
        close_job(input.job); return AZ_STALE;
    }
    if (!prepare_program(assets, data, program)) { close_job(input.job); return AZ_FAILED; }
    /* A unique output name cannot consume a previous process's crash residue. */
    int output_fd = mkstemp(output);
    if (output_fd < 0) { close_job(input.job); return AZ_FAILED; }
    int output_ok = close(output_fd) == 0;
    if (unlink(output)) output_ok = 0;
    if (!output_ok) { close_job(input.job); return AZ_FAILED; }
    snprintf(bpm, sizeof bpm, "%.8f", input.original_bpm);
    const char *saved_grid = (input.needed & AZ_DATA_GRID) ? "-" : input.analysis_path;
    const char *argv[] = {program, input.audio_path, saved_grid, bpm, output, models, NULL};
    az_launch_opts opts = {.size = sizeof opts, .nice = 19, .mem_mb = 512, .log_path = log};
    int pid = C->launch(ID, program, argv, &opts);
    rc = AZ_FAILED;
    if (pid > 0) {
        struct timespec started, now;
        clock_gettime(CLOCK_MONOTONIC, &started);
        while (C->launch_alive(ID, pid) > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (atomic_load(&job->cancelled) || !C->row_value(enabled_row) || now.tv_sec - started.tv_sec >= 600) {
                C->launch_stop(ID, pid); break;
            }
            pause_worker();
        }
        if (!atomic_load(&job->cancelled) && C->row_value(enabled_row)) {
            FILE *file = fopen(output, "rb");
            if (file) {
                struct stat st;
                if (!fstat(fileno(file), &st) && S_ISREG(st.st_mode) && st.st_size >= 16 && st.st_size <= RESULT_MAX) {
                    size_t n = (size_t)st.st_size;
                    uint8_t *bytes = malloc(n);
                    if (bytes && fread(bytes, 1, n, file) == n) {
                        do { rc = L->analysis_commit(ID, input.job, bytes, (uint32_t)n); if (rc == AZ_BUSY) pause_worker(); }
                        while (rc == AZ_BUSY && !atomic_load(&job->cancelled));
                    }
                    free(bytes);
                }
                fclose(file);
            }
        } else rc = AZ_STALE;
    }
    unlink(output);
    if (rc == AZ_OK) pending_link(&input);
    if (rc != AZ_OK) close_job(input.job);
    return rc;
}
static void *worker(void *unused) {
    (void)unused;
    unsigned spool_round = 0;
    for (;;) {
        for (int deck = 0; deck < 4; deck++) {
            int phase = 1;
            if (!atomic_compare_exchange_strong(&work[deck].phase, &phase, 2)) continue;
            work[deck].result = atomic_load(&work[deck].cancelled) ? AZ_STALE : analyze(&work[deck]);
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
                const char *message = job->result == AZ_OK ? "Phrases saved to USB" :
                    job->result == AZ_NOT_FOUND ? "Already has phrases" :
                    job->result == AZ_UNSUPPORTED ? "Unsupported audio or media source" :
                    job->result == AZ_STALE ? "Source changed or analysis cancelled" : "Analysis failed; see analysis.log";
                snprintf(status_text[deck], sizeof status_text[deck], "Deck %d: %s", deck + 1, message);
                if (job->result == AZ_OK) C->banner(ID, status_text[deck], 3000);
                if (retryable(job->result) && d->loaded && !atomic_load(&job->cancelled)) d->attempted = 0;
            }
            atomic_store(&job->phase, 0); phase = 0;
        }
        if (!C->row_value(enabled_row)) { atomic_store(&job->cancelled, 1); d->attempted = 0; continue; }
        if (!phase && d->loaded && d->epoch && d->attempted != d->epoch) {
            job->track = d->track; job->epoch = d->epoch; d->attempted = d->epoch;
            atomic_store(&job->cancelled, 0);
            snprintf(status_text[deck], sizeof status_text[deck], "Deck %d: Checking phrase analysis", deck + 1);
            atomic_store(&job->phase, 1);
        }
    }
}
static void draw(az_draw *draw, az_rect area, void *user) {
    (void)user;
    C->draw_text(draw, area.x + 16, area.y + 16, "Track Analysis", 26, C->color(AZ_COL_TEXT));
    for (int deck = 0; deck < 4; deck++)
        C->draw_text(draw, area.x + 16, area.y + 64 + deck * 38,
            status_text[deck][0] ? status_text[deck] : "Waiting for a USB library track", 20, C->color(AZ_COL_TEXT_DIM));
}
static int init(const az_core_v1 *core) {
    if (!core || core->abi != AZ_MOD_ABI || !AZ_HAS(core, library_service) || !AZ_HAS(core, thread_start) ||
        !AZ_HAS(core, launch) || !AZ_HAS(core, launch_alive) || !AZ_HAS(core, launch_stop) ||
        !AZ_HAS(core, register_row) || !AZ_HAS(core, register_screen) || !AZ_HAS(core, assets_dir) ||
        !AZ_HAS(core, data_dir) || !AZ_HAS(core, banner) || !AZ_HAS(core, lib_spool_kick)) return -1;
    C = core; L = C->library_service(AZ_LIBRARY_ABI);
    if (!AZ_LIB_HAS(L, analysis_open) || !AZ_LIB_HAS(L, analysis_commit) || !AZ_LIB_HAS(L, analysis_close) ||
        !AZ_LIB_HAS(L, on_deck_status) || !AZ_LIB_HAS(L, deck_status_release)) return -2;
    az_row_v1 row = {.size = sizeof row, .key = "enabled", .label = "Track Analysis", .kind = AZ_ROW_TOGGLE,
        .flags = AZ_ROW_DEFAULT_ON, .help = "Generate missing USB phrase analysis automatically on track load."};
    enabled_row = C->register_row(ID, &row);
    if (enabled_row < 0) return -3;
    az_screen_v1 screen = {.size = sizeof screen, .on_draw = draw};
    if (C->register_screen(ID, &screen)) return -4;
    az_deck_watch_v1 watch = {.size = sizeof watch, .version = AZ_DATA_VERSION,
        .deck_mask = AZ_STATUS_ALL_DECKS, .topics = AZ_STATUS_TOPIC(AZ_STATUS_LOAD)};
    if (L->on_deck_status(ID, &watch, deck_status, NULL, &subscription)) return -5;
    return C->thread_start(ID, "track-analysis", worker, NULL) ? -6 : 0;
}
static const char *const firmware[] = {"any", NULL};
static const az_mod_v1 mod = {.abi = AZ_MOD_ABI, .size = sizeof mod, .id = ID, .fw_tags = firmware, .init = init, .tick = tick};
AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void) { return &mod; }
