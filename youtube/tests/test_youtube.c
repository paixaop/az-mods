/* SPDX-License-Identifier: GPL-3.0-only */
#include "files.h"
#include "job.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char id[] = "aqz-KE-bpKQ";
static const char results[] = "["
    "{\"id\":\"aqz-KE-bpKQ\",\"title\":\"Big Buck Bunny\",\"channel\":\"Blender\",\"duration_secs\":634,"
    "\"audio\":{\"itag\":140,\"bitrate\":129481,\"sample_rate\":44100,\"channels\":2}},"
    "{\"id\":\"YE7VzlLtp-4\",\"title\":\"Line\\nbreak\\u0000end\"},"
    "{\"id\":\"aqz-KE-bpKQ\",\"title\":\"Live\",\"is_live\":true},"
    "{\"id\":\"../invalid\"},{\"id\":\"_--abcdef01\"}]";

#define AAC_FIELDS "\"container\":\"mp4\",\"vcodec\":\"none\",\"acodec\":\"mp4a.40.2\",\"has_audio\":true,"
#define AUDIO_ROW(TAG, RATE, CHANNELS, BITRATE, DRC) "{" AAC_FIELDS "\"itag\":" #TAG "," \
    "\"audio_sample_rate\":" #RATE ",\"audio_channels\":" #CHANNELS ",\"average_bitrate\":" #BITRATE ",\"is_drc\":" #DRC "}"

static const char formats[] = "{\"id\":\"aqz-KE-bpKQ\",\"formats\":["
    "{\"itag\":251,\"acodec\":\"opus\",\"container\":\"webm\",\"has_audio\":true,\"vcodec\":\"none\"},"
    "{\"itag\":22,\"has_audio\":true,\"vcodec\":\"avc1\",\"container\":\"mp4\",\"acodec\":\"mp4a.40.2\"},"
    "{\"itag\":140,\"has_audio\":true,\"vcodec\":\"none\",\"container\":\"mp4\",\"acodec\":\"mp4a.40.5\"},"
    AUDIO_ROW(0, 44100, 1, 90000, false) ","
    AUDIO_ROW(120, 32000, 2, 190000, false) ","
    AUDIO_ROW(140, 44100, 2, 129481, false) ","
    AUDIO_ROW(139, 48000, 1, 160000, false) ","
    AUDIO_ROW(140, 48000, 2, 210000, true) ","
    AUDIO_ROW(141, 48000, 2, 180000, false) ","
    AUDIO_ROW(22, 48000, 2, 290000, false) ","
    "{" AAC_FIELDS "\"itag\":141,\"audio_channels\":2,\"audio_sample_rate\":48000,\"bitrate\":175000}]}";

static void assert_zero_tail(const char *text, size_t size)
{
    for (size_t i = strlen(text); i < size; ++i)
        assert(text[i] == 0);
}

static void metadata_cases(void)
{
    YtTrack tracks[YT_RESULT_LIMIT];
    memset(tracks, 0xa5, sizeof tracks);
    assert(yt_valid_id(id));
    assert(!yt_valid_id(NULL) && !yt_valid_id("aqz-KE-bpKQx") && !yt_valid_id("abc/1234567"));
    assert(yt_search_results(results, strlen(results), tracks, YT_RESULT_LIMIT) == 3);
    assert(tracks[0].duration_s == 634 && !strcmp(tracks[0].channel, "Blender"));
    assert(!strcmp(tracks[1].title, "Line break end"));
    assert(!strcmp(tracks[2].title, tracks[2].id));
    for (int i = 0; i < 3; ++i) {
        assert_zero_tail(tracks[i].title, sizeof tracks[i].title);
        assert_zero_tail(tracks[i].channel, sizeof tracks[i].channel);
    }
    assert(yt_search_results(results, strlen(results), tracks, 1) == 1);
    assert(yt_search_results("[]", 2, tracks, YT_RESULT_LIMIT) == 0);
    assert(yt_search_results("{}", 2, tracks, YT_RESULT_LIMIT) == -1);
    assert(yt_search_results(NULL, 3, tracks, 1) == -1);
    assert(yt_search_results(results, YT_METADATA_LIMIT + 1, tracks, YT_RESULT_LIMIT) == -1);
    assert(yt_search_results(results, strlen(results), NULL, 1) == -1);

    char long_title[800];
    memset(long_title, 0, sizeof(long_title));
    size_t used = (size_t)snprintf(long_title, sizeof(long_title), "[{\"id\":\"%s\",\"title\":\"", id);
    for (int i = 0; i < 120; ++i) {
        memcpy(long_title + used, "é", 2);
        used += 2;
    }
    memcpy(long_title + used, "\"}]", 4);
    assert(yt_search_results(long_title, strlen(long_title), tracks, 1) == 1);
    assert(strlen(tracks[0].title) == 190);

    YtFormat format;
    assert(yt_pick_audio(formats, strlen(formats), id, &format) == 0);
    assert(format.itag == 141 && format.bitrate == 180000 && format.sample_rate == 48000);
    assert(yt_pick_audio(formats, strlen(formats), "YE7VzlLtp-4", &format) == -1);
    assert(yt_pick_audio(formats, strlen(formats), NULL, &format) == -1);
    assert(yt_pick_audio(formats, strlen(formats), id, NULL) == -1);
    static const char live[] = "{\"id\":\"aqz-KE-bpKQ\",\"is_live\":true,\"formats\":["
                               AUDIO_ROW(140, 44100, 2, 100000, false) "]}";
    assert(yt_pick_audio(live, strlen(live), id, &format) == -1);
}

static void put(const char *directory, const char *name, const char *value)
{
    char path[PATH_MAX];
    assert(yt_join_path(path, sizeof(path), directory, name) == 0);
    FILE *file = fopen(path, "w");
    assert(file && fputs(value, file) >= 0 && fclose(file) == 0);
}

static void remove_file(const char *directory, const char *name)
{
    char path[PATH_MAX];
    assert(yt_join_path(path, sizeof(path), directory, name) == 0);
    assert(unlink(path) == 0);
}

static void catalog_cases(const char *root)
{
    char directory[PATH_MAX], path[PATH_MAX];
    assert(yt_join_path(directory, sizeof(directory), root, "catalog") == 0);
    assert(yt_join_path(path, 3, root, "long") == -1);
    int more;
    YtTrack entries[2];
    assert(yt_list_tracks(directory, 0, entries, 2, &more) == 0);
    assert(mkdir(directory, 0700) == 0);
    put(directory, "aqz-KE-bpKQ.m4a", "completed audio fixture");
    put(directory, "YE7VzlLtp-4.m4a", "other completed audio fixture");
    put(directory, "_--abcdef01.m4a", "");
    put(directory, "xqz-KE-bpKQ.m4a.part", "incomplete");
    assert(yt_join_path(path, sizeof(path), directory, "other.json") == 0);
    assert(symlink("aqz-KE-bpKQ.m4a", path) == 0);
    size_t length;
    assert(!yt_read_metadata(path, &length));
    put(directory, "aqz-KE-bpKQ.txt", "Big Buck Bunny\nBlender\n634\n");
    assert(yt_audio_ready(directory, id) && !yt_audio_ready(directory, "_--abcdef01"));
    assert(yt_list_tracks(directory, 0, entries, 2, &more) == 2 && !more);
    int known = !strcmp(entries[0].id, id) ? 0 : 1;
    assert(!strcmp(entries[known].title, "Big Buck Bunny") && entries[known].duration_s == 634);
    assert(!strcmp(entries[1 - known].title, "YE7VzlLtp-4"));
    assert(yt_list_tracks(directory, 0, entries, 1, &more) == 1 && more);
    assert(yt_list_tracks(directory, 1, entries, 1, &more) == 1 && !more);
    remove_file(directory, "aqz-KE-bpKQ.m4a");
    remove_file(directory, "YE7VzlLtp-4.m4a");
    remove_file(directory, "_--abcdef01.m4a");
    remove_file(directory, "xqz-KE-bpKQ.m4a.part");
    remove_file(directory, "aqz-KE-bpKQ.txt");
    remove_file(directory, "other.json");
    assert(rmdir(directory) == 0);
}

static char fixture[] = "/tmp/az-youtube-test.XXXXXX";
static az_source_provider_v1 provider;
static az_row_v1 storage_row;
static enum az_source_storage_mode active_mode;
static unsigned launches, downloads;
static unsigned waits, stop_requests, pending_exit;
static unsigned exit_delay = 27;
static bool timeout_download, refuse_stop;
static char timed_out_job[PATH_MAX];
static int mount_present = 1, download_status, missing_audio, legacy_source;
static int saved_choice, menu_choice, reject_mode, register_row_status;

static int load_choice(const char *mod_id, const char *key, int fallback)
{
    assert(!strcmp(mod_id, YT_MOD_ID) && !strcmp(key, "storage-mode") && fallback == 0);
    return saved_choice;
}

static void save_choice(const char *mod_id, const char *key, int value)
{
    assert(!strcmp(mod_id, YT_MOD_ID) && !strcmp(key, "storage-mode"));
    saved_choice = value;
}

static int register_row(const char *mod_id, const az_row_v1 *row)
{
    assert(!strcmp(mod_id, YT_MOD_ID) && !strcmp(row->key, "storage-mode"));
    assert(row->kind == AZ_ROW_CHOICE && row->n_choices == 2);
    assert(!strcmp(row->choices[0], "Streaming") && !strcmp(row->choices[1], "Import"));
    storage_row = *row;
    menu_choice = row->default_choice;
    return register_row_status;
}

static int row_set(int row, int value)
{
    assert(row == register_row_status);
    if (value != menu_choice) {
        menu_choice = value;
        storage_row.on_change(value, storage_row.user);
    }
    return 0;
}

static const char *data_dir(const char *mod_id)
{
    assert(!strcmp(mod_id, YT_MOD_ID));
    return mount_present ? fixture : NULL;
}

static int timed_out_launch(const char *directory)
{
    waits = stop_requests = 0;
    snprintf(timed_out_job, sizeof timed_out_job, "%s", directory);
    return 28;
}

static int launch(const char *mod_id, const char *exe, const char *const *args, const az_launch_opts *options)
{
    assert(!strcmp(mod_id, YT_MOD_ID) && strstr(exe, "/youtube-helper"));
    assert(options && options->mem_mb && options->nice);
    ++launches;
    if (!strcmp(args[0], "search")) {
        assert(!args[3]);
        assert(!strncmp(args[1], "/tmp/az-youtube-search.", sizeof "/tmp/az-youtube-search." - 1));
        if (!strcmp(args[2], "refuse"))
            return -1;
        if (!strcmp(args[2], "timeout")) {
            put(args[1], "search.json", results);
            return timed_out_launch(args[1]);
        }
        if (!strcmp(args[2], "corrupt"))
            put(args[1], "search.json", "{}");
        else {
            assert(!strcmp(args[2], "q"));
            put(args[1], "search.json", results);
        }
    } else {
        assert(!strcmp(args[0], "download") && !strcmp(args[2], id) && !args[4]);
        assert(!strcmp(args[1], args[3]));
        assert(!strncmp(args[3], "/tmp/az-youtube-download.", sizeof "/tmp/az-youtube-download." - 1));
        ++downloads;
        if (timeout_download) {
            put(args[3], "aqz-KE-bpKQ.m4a", "unconfirmed audio fixture");
            return timed_out_launch(args[3]);
        }
        if (download_status) {
            char code[] = {(char)('0' + download_status), '\0'};
            put(args[1], "status", code);
            return 27;
        }
        if (!missing_audio)
            put(args[3], "aqz-KE-bpKQ.m4a", "downloaded audio fixture");
    }
    put(args[1], "status", "0");
    return 27;
}

static int launch_alive(const char *mod_id, int pid)
{
    assert(!strcmp(mod_id, YT_MOD_ID) && (pid == 27 || pid == 28));
    if (pid == 28) {
        assert(access(timed_out_job, F_OK) == 0);
        if (!stop_requests)
            return 1;
        if (pending_exit) {
            --pending_exit;
            return 1;
        }
    }
    return 0;
}

static int launch_stop(const char *mod_id, int pid)
{
    assert(!strcmp(mod_id, YT_MOD_ID) && pid == 28 && !stop_requests);
    ++stop_requests;
    pending_exit = exit_delay;
    put(timed_out_job, "status.tmp", "1");
    return refuse_stop ? -1 : 0;
}

/* Advance helper poll delays without waiting through the real deadline. */
int nanosleep(const struct timespec *delay, struct timespec *remaining)
{
    assert(delay->tv_sec == 0 && delay->tv_nsec == 75000000 && !remaining);
    ++waits;
    return 0;
}

static int register_provider(const char *mod_id, const az_source_provider_v1 *value)
{
    assert(!strcmp(mod_id, YT_MOD_ID) && !strcmp(value->name, "YouTube"));
    assert(value->size == sizeof(*value));
    assert(value->icon_png && value->icon_bytes > 64);
    provider = *value;
    active_mode = value->storage_mode;
    return AZ_OK;
}

static int set_storage_mode(const char *mod_id, enum az_source_storage_mode mode)
{
    assert(!strcmp(mod_id, YT_MOD_ID));
    if (reject_mode)
        return AZ_UNSUPPORTED;
    active_mode = mode;
    return AZ_OK;
}

static const az_source_v1 *source_service(uint32_t abi)
{
    assert(abi == AZ_SOURCE_ABI);
    static const az_source_v1 source = {.abi = AZ_SOURCE_ABI, .size = sizeof(source),
        .register_provider = register_provider, .set_storage_mode = set_storage_mode};
    static const az_source_v1 legacy = {.abi = AZ_SOURCE_ABI,
        .size = offsetof(az_source_v1, register_provider)};
    return legacy_source ? &legacy : &source;
}

static void search_cases(void)
{
    az_source_page_v1 page;
    az_source_track_v1 track[2];
    unsigned previous_launches = launches;
    assert(provider.search(provider.user, "", 0, track, 2, &page) == AZ_OK);
    assert(page.count == 0 && !page.has_more && launches == previous_launches);
    assert(provider.search(provider.user, "refuse", 0, track, 2, &page) == AZ_FAILED);
    assert(provider.search(provider.user, "corrupt", 0, track, 2, &page) == AZ_FAILED);
    assert(provider.search(provider.user, "q", 0, track, 2, &page) == AZ_OK);
    assert(page.count == 2 && page.has_more);
    assert(!strcmp(track[0].id, id) && !strcmp(track[0].title, "Big Buck Bunny"));
    assert(!strcmp(track[0].artist, "Blender") && track[0].duration_ms == 634000u);
    assert(!strcmp(track[1].id, "YE7VzlLtp-4"));
    assert_zero_tail(track[0].title, sizeof track[0].title);
    assert_zero_tail(track[1].artist, sizeof track[1].artist);
    assert(provider.search(provider.user, "q", 2, track, 2, &page) == AZ_OK);
    assert(page.count == 1 && !page.has_more && !strcmp(track[0].id, "_--abcdef01"));
    assert(provider.search(provider.user, "q", UINT32_MAX, track, 2, &page) == AZ_OK);
    assert(page.count == 0 && !page.has_more);
    assert(provider.search(provider.user, "timeout", 0, track, 2, &page) == AZ_CANCELLED);
    assert(!page.count && stop_requests == 1 && waits == 1827 && !pending_exit);
    assert(access(timed_out_job, F_OK) == -1);
}

static void playlist_cases(void)
{
    az_source_page_v1 page;
    az_source_playlist_v1 playlist;
    az_source_track_v1 track;
    assert(provider.list_playlists(provider.user, 0, &playlist, 1, &page) == AZ_OK);
    assert(page.count == 1 && !page.has_more && !strcmp(playlist.name, YT_DOWNLOADS));
    assert(provider.list_playlists(provider.user, 1, &playlist, 1, &page) == AZ_OK);
    assert(page.count == 0 && !page.has_more);
    assert(provider.playlist_tracks(provider.user, "unknown", 0, &track, 1, &page) == AZ_NOT_FOUND);
    assert(provider.playlist_tracks(provider.user, playlist.id, 0, &track, 1, &page) == AZ_OK);
    assert(page.count == 0 && !page.has_more);
}

static void assert_legacy_track(void)
{
    az_source_page_v1 page;
    az_source_track_v1 track;
    assert(provider.playlist_tracks(provider.user, YT_DOWNLOADS_ID, 0, &track, 1, &page) == AZ_OK);
    assert(page.count == 1 && !page.has_more && !strcmp(track.id, id));
    assert(!strcmp(track.title, "Big Buck Bunny") && !strcmp(track.artist, "Blender Team"));
    assert(track.duration_ms == 634000u);
}

static void stage_legacy_download(const az_source_track_v1 *track, const char *stage)
{
    char path[PATH_MAX], downloads_path[PATH_MAX];
    assert(yt_join_path(downloads_path, sizeof(downloads_path), fixture, YT_DOWNLOADS) == 0);
    assert(mkdir(downloads_path, 0700) == 0);
    put(downloads_path, "aqz-KE-bpKQ.m4a", "legacy audio fixture");
    put(downloads_path, "aqz-KE-bpKQ.txt", "Big Buck Bunny\nBlender Team\n634\n");
    assert_legacy_track();
    unsigned previous_downloads = downloads;
    assert(provider.prepare_track(provider.user, track, stage, path, sizeof(path)) == AZ_OK);
    assert(downloads == previous_downloads);
    size_t length = 0;
    char *audio = yt_read_metadata(path, &length);
    assert(audio && length == strlen("legacy audio fixture") && !strcmp(audio, "legacy audio fixture"));
    free(audio);
    assert(provider.prepare_track(provider.user, track, stage, path, sizeof(path)) == AZ_FAILED);
    assert(downloads == previous_downloads);
    assert(unlink(path) == 0);
    assert_legacy_track();
    remove_file(downloads_path, "aqz-KE-bpKQ.m4a");
    remove_file(downloads_path, "aqz-KE-bpKQ.txt");
    assert(rmdir(downloads_path) == 0);
}

static void timeout_download_cases(const az_source_track_v1 *track, const char *stage)
{
    char path[PATH_MAX] = "", final[PATH_MAX];
    assert(yt_audio_path(final, sizeof final, stage, track->id) == 0);
    timeout_download = refuse_stop = true;
    assert(provider.prepare_track(provider.user, track, stage, path, sizeof path) == AZ_CANCELLED);
    assert(stop_requests == 1 && waits == 1827 && !pending_exit);
    assert(access(timed_out_job, F_OK) == -1 && access(final, F_OK) == -1 && !path[0]);

    exit_delay = 100;
    refuse_stop = false;
    assert(provider.prepare_track(provider.user, track, stage, path, sizeof path) == AZ_CANCELLED);
    assert(stop_requests == 1 && waits == 1840 && pending_exit > 0);
    assert(access(timed_out_job, F_OK) == 0 && access(final, F_OK) == -1 && !path[0]);
    remove_file(timed_out_job, "aqz-KE-bpKQ.m4a");
    remove_file(timed_out_job, "status.tmp");
    assert(rmdir(timed_out_job) == 0);
    timeout_download = false;
    exit_delay = 27;
}

static void download_cases(void)
{
    char path[PATH_MAX], stage[PATH_MAX], legacy_path[PATH_MAX];
    assert(yt_join_path(stage, sizeof(stage), fixture, "stage") == 0);
    assert(mkdir(stage, 0700) == 0);
    assert(yt_join_path(legacy_path, sizeof(legacy_path), fixture, YT_DOWNLOADS) == 0);
    /* An ID from a native playlist can reach LOAD before any search was run. */
    const az_source_track_v1 track = {.id = "aqz-KE-bpKQ", .title = "Big Buck Bunny",
        .artist = "Blender\nTeam", .duration_ms = 634000u};
    az_source_track_v1 invalid = {.id = "../bad"};
    assert(provider.prepare_track(provider.user, &invalid, stage, path, sizeof(path)) == AZ_INVALID);
    assert(provider.prepare_track(provider.user, &track, stage, path, 8) == AZ_LIMIT);
    mount_present = 0;
    assert(provider.prepare_track(provider.user, &track, stage, path, sizeof(path)) == AZ_NOT_FOUND);
    mount_present = 1;
    assert(downloads == 0);
    download_status = YT_HELPER_INVALID;
    assert(provider.prepare_track(provider.user, &track, stage, path, sizeof(path)) == AZ_INVALID);
    download_status = YT_HELPER_TRANSFER;
    assert(provider.prepare_track(provider.user, &track, stage, path, sizeof(path)) == AZ_FAILED);
    download_status = YT_HELPER_OK;
    missing_audio = 1;
    assert(provider.prepare_track(provider.user, &track, stage, path, sizeof(path)) == AZ_FAILED);
    missing_audio = 0;
    assert(provider.prepare_track(provider.user, &track, stage, path, sizeof(path)) == AZ_OK);
    assert(downloads == 4);
    assert(path[0] == '/' && strstr(path, "/stage/aqz-KE-bpKQ.m4a"));
    assert(access(path, R_OK) == 0 && access(legacy_path, F_OK) == -1);
    assert(unlink(path) == 0);
    stage_legacy_download(&track, stage);
    timeout_download_cases(&track, stage);
    assert(rmdir(stage) == 0);
}

static void workflow(void)
{
    const az_core_v1 legacy = {.size = offsetof(az_core_v1, source_service)};
    const az_core_v1 core = {.size = sizeof(core), .source_service = source_service,
        .assets_dir = data_dir, .data_dir = data_dir, .launch = launch, .launch_alive = launch_alive,
        .launch_stop = launch_stop, .cfg_get = load_choice, .cfg_set = save_choice,
        .register_row = register_row, .row_set = row_set};
    az_core_v1 incomplete = core;
    incomplete.launch_stop = NULL;
    const az_mod_v1 *mod = az_mod_v1_entry();
    assert(mod->init(&legacy) == AZ_UNSUPPORTED);
    assert(mod->init(&incomplete) == AZ_UNSUPPORTED);
    legacy_source = 1;
    assert(mod->init(&core) == AZ_UNSUPPORTED);
    legacy_source = 0;
    register_row_status = -1;
    assert(mod->init(&core) == AZ_FAILED);
    register_row_status = 27;
    assert(mod->init(&core) == AZ_OK && !mod->tick);
    assert(active_mode == AZ_SOURCE_STREAMING && menu_choice == 0);
    assert(row_set(register_row_status, 1) == 0);
    assert(active_mode == AZ_SOURCE_IMPORT && saved_choice == 1);
    reject_mode = 1;
    assert(row_set(register_row_status, 0) == 0);
    assert(active_mode == AZ_SOURCE_IMPORT && saved_choice == 1 && menu_choice == 1);
    reject_mode = 0;
    assert(mod->init(&core) == AZ_OK);
    assert(active_mode == AZ_SOURCE_IMPORT && menu_choice == 1);
    playlist_cases();
    download_cases();
    search_cases();
}

int main(void)
{
    assert(mkdtemp(fixture));
    metadata_cases();
    catalog_cases(fixture);
    workflow();
    assert(rmdir(fixture) == 0);
    puts("YouTube metadata, native provider callbacks, and download reuse passed");
    return 0;
}
