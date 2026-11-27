/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef YOUTUBE_FILES_H
#define YOUTUBE_FILES_H

#include "model.h"

#define YT_DOWNLOADS "Downloads"

int yt_join_path(char *path, size_t capacity, const char *directory, const char *name);
char *yt_read_metadata(const char *path, size_t *length);
int yt_audio_path(char *path, size_t capacity, const char *dir, const char *id);
int yt_audio_ready(const char *dir, const char *id);
/* 0 copied, 1 no complete legacy download, -1 a copy failed. */
int yt_stage_cached_track(const char *directory, const char *id, const char *destination);
int yt_list_tracks(const char *directory, size_t offset, YtTrack *items, size_t capacity, int *more);

#endif
