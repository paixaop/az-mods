/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef YOUTUBE_JOB_H
#define YOUTUBE_JOB_H

#include "azmod-mod.h"

#define YT_MOD_ID "youtube"
#define YT_DOWNLOADS_ID "downloads"

int yt_job_services(const az_core_v1 *api);
int yt_list_playlists(void *user, uint32_t offset, az_source_playlist_v1 *rows,
                      uint32_t capacity, az_source_page_v1 *page);
int yt_playlist_tracks(void *user, const char *playlist_id, uint32_t offset,
                       az_source_track_v1 *rows, uint32_t capacity, az_source_page_v1 *page);
int yt_search_tracks(void *user, const char *query, uint32_t offset, az_source_track_v1 *rows,
                     uint32_t capacity, az_source_page_v1 *page);
int yt_prepare_track(void *user, const az_source_track_v1 *track, const char *directory,
                     char *path, uint32_t capacity);

#endif
