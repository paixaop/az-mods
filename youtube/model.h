/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef YOUTUBE_MODEL_H
#define YOUTUBE_MODEL_H

#include <stddef.h>

enum { YT_ID_SIZE = 12, YT_RESULT_LIMIT = 24, YT_METADATA_LIMIT = 1024 * 1024 };

typedef struct {
    int itag;
    int bitrate;
    int sample_rate;
    int channels;
    int drc;
} YtFormat;

typedef struct {
    char id[YT_ID_SIZE];
    char title[192];
    char channel[96];
    int duration_s;
} YtTrack;

enum YtHelperStatus {
    YT_HELPER_OK, YT_HELPER_FAILED, YT_HELPER_INVALID,
    YT_HELPER_TRANSFER, YT_HELPER_REMUX, YT_HELPER_USB,
};

int yt_valid_id(const char *id);
int yt_audio_itag(int itag);
int yt_search_results(const char *json, size_t length, YtTrack *tracks, size_t capacity);
int yt_pick_audio(const char *json, size_t length, const char *id, YtFormat *out);

#endif
