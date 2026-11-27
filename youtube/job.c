/* SPDX-License-Identifier: GPL-3.0-only */
#define _XOPEN_SOURCE 700
#include "job.h"
#include "files.h"

#include <ftw.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void brief_wait(void)
{
    const struct timespec delay = {.tv_nsec = 75000000};
    nanosleep(&delay, NULL);
}

static int remove_result(const char *path, const struct stat *st, int type, struct FTW *info)
{
    (void)st;
    (void)type;
    (void)info;
    (void)remove(path);
    return 0;
}

static void clean_result(const char *directory)
{
    (void)nftw(directory, remove_result, 8, FTW_DEPTH | FTW_MOUNT | FTW_PHYS);
}

static int helper_result(const char *directory)
{
    char path[PATH_MAX];
    if (yt_join_path(path, sizeof(path), directory, "status") < 0)
        return AZ_FAILED;
    size_t length;
    char *status = yt_read_metadata(path, &length);
    int result = status && length == 1 ? status[0] - '0' : YT_HELPER_FAILED;
    free(status);
    return result >= YT_HELPER_OK && result <= YT_HELPER_USB ? result : AZ_FAILED;
}

static bool wait_for_exit(const az_core_v1 *core, int pid, unsigned polls)
{
    for (unsigned i = 0; i < polls; ++i) {
        if (!core->launch_alive(YT_MOD_ID, pid))
            return true;
        brief_wait();
    }
    return !core->launch_alive(YT_MOD_ID, pid);
}

static int await_helper(const az_core_v1 *core, int pid, const char *directory, bool *can_clean)
{
    if (wait_for_exit(core, pid, 1800))
        return helper_result(directory);
    (void)core->launch_stop(YT_MOD_ID, pid);
    /* A signal, including a refused stop, does not establish exit. Allow for
     * the SDK's two-second SIGKILL grace period before considering cleanup. */
    *can_clean = wait_for_exit(core, pid, 40);
    return AZ_CANCELLED;
}

static int launch_job(const az_core_v1 *core, const char *const *args, const char *directory,
                      bool *can_clean)
{
    const char *assets = core->assets_dir(YT_MOD_ID);
    char path[PATH_MAX];
    if (!assets || yt_join_path(path, sizeof(path), assets, "youtube-helper") < 0)
        return AZ_NOT_FOUND;
    const az_launch_opts options = {.size = sizeof(options), .nice = 10, .mem_mb = 128};
    int pid = core->launch(YT_MOD_ID, path, args, &options);
    return pid >= 0 ? await_helper(core, pid, directory, can_clean) : AZ_FAILED;
}

static int downloads_path(const az_core_v1 *core, char downloads[PATH_MAX])
{
    const char *data = core->data_dir(YT_MOD_ID);
    if (!data)
        return AZ_NOT_FOUND;
    return yt_join_path(downloads, PATH_MAX, data, YT_DOWNLOADS) ? AZ_LIMIT : AZ_OK;
}

static int job_status(int status)
{
    switch (status) {
    case YT_HELPER_OK: return AZ_OK;
    case YT_HELPER_INVALID: return AZ_INVALID;
    case AZ_NOT_FOUND: return AZ_NOT_FOUND;
    case AZ_CANCELLED: return AZ_CANCELLED;
    default: return AZ_FAILED;
    }
}

static uint32_t duration_ms(int seconds)
{
    return (uint32_t)seconds <= UINT32_MAX / 1000u ? (uint32_t)seconds * 1000u : 0;
}

static void copy_track(az_source_track_v1 *out, const YtTrack *track)
{
    *out = (az_source_track_v1){.duration_ms = duration_ms(track->duration_s)};
    memcpy(out->id, track->id, sizeof(track->id));
    memcpy(out->title, track->title, sizeof(track->title));
    memcpy(out->artist, track->channel, sizeof(track->channel));
}

static void copy_rows(az_source_track_v1 *out, const YtTrack *tracks, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        copy_track(&out[i], &tracks[i]);
}

int yt_list_playlists(void *user, uint32_t offset, az_source_playlist_v1 *rows,
                      uint32_t capacity, az_source_page_v1 *page)
{
    (void)user;
    (void)capacity;
    *page = (az_source_page_v1){0};
    if (offset == 0) {
        *rows = (az_source_playlist_v1){.id = YT_DOWNLOADS_ID, .name = YT_DOWNLOADS};
        page->count = 1;
    }
    return AZ_OK;
}

int yt_playlist_tracks(void *user, const char *playlist_id, uint32_t offset,
                       az_source_track_v1 *rows, uint32_t capacity, az_source_page_v1 *page)
{
    *page = (az_source_page_v1){0};
    if (strcmp(playlist_id, YT_DOWNLOADS_ID))
        return AZ_NOT_FOUND;
    char downloads[PATH_MAX];
    int status = downloads_path(user, downloads);
    if (status != AZ_OK)
        return status;

    YtTrack tracks[AZ_SOURCE_PAGE_MAX];
    int more;
    int count = yt_list_tracks(downloads, offset, tracks, capacity, &more);
    if (count < 0)
        return AZ_FAILED;
    copy_rows(rows, tracks, (uint32_t)count);
    *page = (az_source_page_v1){.count = (uint32_t)count, .has_more = (uint32_t)more};
    return AZ_OK;
}

static int read_search(const char *directory, YtTrack tracks[YT_RESULT_LIMIT])
{
    char path[PATH_MAX];
    if (yt_join_path(path, sizeof(path), directory, "search.json"))
        return -1;
    size_t length;
    char *data = yt_read_metadata(path, &length);
    if (!data)
        return -1;
    int count = yt_search_results(data, length, tracks, YT_RESULT_LIMIT);
    free(data);
    return count;
}

static int search(const az_core_v1 *core, const char *query, YtTrack tracks[YT_RESULT_LIMIT],
                  int *count)
{
    char directory[] = "/tmp/az-youtube-search.XXXXXX";
    if (!mkdtemp(directory))
        return AZ_FAILED;
    const char *const args[] = {"search", directory, query, NULL};
    bool can_clean = true;
    int status = launch_job(core, args, directory, &can_clean);
    if (status == YT_HELPER_OK)
        *count = read_search(directory, tracks);
    if (can_clean)
        clean_result(directory);
    return job_status(status);
}

static void search_page(const YtTrack *tracks, uint32_t total, uint32_t offset,
                        az_source_track_v1 *rows, uint32_t capacity, az_source_page_v1 *page)
{
    if (offset >= total)
        return;
    uint32_t remaining = total - offset;
    uint32_t count = remaining < capacity ? remaining : capacity;
    copy_rows(rows, tracks + offset, count);
    *page = (az_source_page_v1){.count = count, .has_more = count < remaining};
}

int yt_search_tracks(void *user, const char *query, uint32_t offset, az_source_track_v1 *rows,
                     uint32_t capacity, az_source_page_v1 *page)
{
    *page = (az_source_page_v1){0};
    if (!query[0] || offset >= YT_RESULT_LIMIT)
        return AZ_OK;
    YtTrack tracks[YT_RESULT_LIMIT];
    int count = -1;
    int status = search(user, query, tracks, &count);
    if (status != AZ_OK)
        return status;
    if (count < 0)
        return AZ_FAILED;
    search_page(tracks, (uint32_t)count, offset, rows, capacity, page);
    return AZ_OK;
}

static int download(const az_core_v1 *core, const char *id, const char *final)
{
    /* The SDK may remove its staging directory as soon as this returns. Keep
     * it outside the helper's reach, even when the helper is slow to stop. */
    char directory[] = "/tmp/az-youtube-download.XXXXXX";
    if (!mkdtemp(directory))
        return AZ_FAILED;
    const char *const args[] = {"download", directory, id, directory, NULL};
    bool can_clean = true;
    int status = launch_job(core, args, directory, &can_clean);
    if (status == YT_HELPER_OK && yt_stage_cached_track(directory, id, final) != 0)
        status = AZ_FAILED;
    if (can_clean)
        clean_result(directory);
    return job_status(status);
}

static int prepare_audio(const az_core_v1 *core, const char *id, const char *final)
{
    char downloads[PATH_MAX];
    if (downloads_path(core, downloads) == AZ_OK) {
        int staged = yt_stage_cached_track(downloads, id, final);
        if (staged <= 0)
            return staged == 0 ? AZ_OK : AZ_FAILED;
    }
    return download(core, id, final);
}

int yt_prepare_track(void *user, const az_source_track_v1 *track, const char *directory,
                     char *path, uint32_t capacity)
{
    if (!yt_valid_id(track->id))
        return AZ_INVALID;
    char final[PATH_MAX];
    if (yt_audio_path(final, sizeof(final), directory, track->id) ||
        strlen(final) >= capacity)
        return AZ_LIMIT;

    int status = prepare_audio(user, track->id, final);
    if (status != AZ_OK)
        return status;
    if (!yt_audio_ready(directory, track->id))
        return AZ_FAILED;
    memcpy(path, final, strlen(final) + 1);
    return AZ_OK;
}

static int process_services(const az_core_v1 *api)
{
    return api->launch && api->launch_alive && api->launch_stop;
}

int yt_job_services(const az_core_v1 *api)
{
    return api->assets_dir && api->data_dir && process_services(api);
}
