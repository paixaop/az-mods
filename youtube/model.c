/* SPDX-License-Identifier: GPL-3.0-only */
#include "model.h"
#include "vendor/cmcp/cmcp_json.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

int yt_valid_id(const char *id)
{
    static const char allowed[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    return id && strspn(id, allowed) == YT_ID_SIZE - 1 && id[YT_ID_SIZE - 1] == '\0';
}

static cmcp_json_t *metadata(const char *json, size_t length)
{
    return json && length <= YT_METADATA_LIMIT ? cmcp_json_parse(json, length) : NULL;
}

static int number(const cmcp_json_t *object, const char *key)
{
    const cmcp_json_t *field = cmcp_json_object_get(object, key);
    if (!field || field->type != CMCP_JSON_INT)
        return 0;
    long long value = cmcp_json_int(field);
    return value > 0 && value <= INT_MAX ? (int)value : 0;
}

static int value_is(const cmcp_json_t *object, const char *key, const char *value)
{
    const cmcp_json_t *field = cmcp_json_object_get(object, key);
    size_t length = strlen(value);
    return cmcp_json_string_len(field) == length &&
           memcmp(cmcp_json_string(field), value, length) == 0;
}

static size_t utf8_prefix(const char *text, size_t length, size_t limit)
{
    if (length <= limit)
        return length;
    while (limit && ((unsigned char)text[limit] & 0xc0) == 0x80)
        --limit;
    return limit;
}

static void copy_text(char *out, size_t capacity, const cmcp_json_t *object, const char *key)
{
    const cmcp_json_t *field = cmcp_json_object_get(object, key);
    const char *text = cmcp_json_string(field);
    size_t length = utf8_prefix(text, cmcp_json_string_len(field), capacity - 1);
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        out[i] = c >= 32 && c != 127 ? (char)c : ' ';
    }
    out[length] = '\0';
}

static int read_track(const cmcp_json_t *entry, YtTrack *track)
{
    const cmcp_json_t *id = cmcp_json_object_get(entry, "id");
    const char *text = cmcp_json_string(id);
    if (cmcp_json_string_len(id) != YT_ID_SIZE - 1 || !yt_valid_id(text))
        return 0;
    if (cmcp_json_bool(cmcp_json_object_get(entry, "is_live")))
        return 0;

    *track = (YtTrack){0};
    memcpy(track->id, text, sizeof(track->id));
    copy_text(track->title, sizeof(track->title), entry, "title");
    copy_text(track->channel, sizeof(track->channel), entry, "channel");
    track->duration_s = number(entry, "duration_secs");
    if (!track->title[0])
        memcpy(track->title, track->id, sizeof(track->id));
    return 1;
}

static int read_results(const cmcp_json_t *list, YtTrack *tracks, size_t capacity)
{
    size_t count = 0;
    size_t total = cmcp_json_array_len(list);
    for (size_t i = 0; i < total && count < capacity; ++i)
        count += read_track(cmcp_json_array_at(list, i), &tracks[count]);
    return (int)count;
}

int yt_search_results(const char *json, size_t length, YtTrack *tracks, size_t capacity)
{
    if (!tracks || capacity > YT_RESULT_LIMIT)
        return -1;
    cmcp_json_t *list = metadata(json, length);
    int count = list && list->type == CMCP_JSON_ARRAY ? read_results(list, tracks, capacity) : -1;
    cmcp_json_free(list);
    return count;
}

static int is_aac_lc(const cmcp_json_t *row)
{
    return value_is(row, "acodec", "mp4a.40.2") && value_is(row, "container", "mp4") &&
           value_is(row, "vcodec", "none") && cmcp_json_bool(cmcp_json_object_get(row, "has_audio")) &&
           !cmcp_json_bool(cmcp_json_object_get(row, "has_video"));
}

int yt_audio_itag(int itag)
{
    /* The second ytcui-dl request selects by itag. Accept only YouTube's
     * mono or stereo AAC-only itags, even if other rows claim the same codec. */
    return itag >= 139 && itag <= 141;
}

static int supported_audio(const YtFormat *format)
{
    return yt_audio_itag(format->itag) && (format->channels == 1 || format->channels == 2) &&
           (format->sample_rate == 44100 || format->sample_rate == 48000);
}

static YtFormat read_format(const cmcp_json_t *row)
{
    YtFormat result = {0};
    if (!is_aac_lc(row))
        return result;
    result.itag = number(row, "itag");
    result.bitrate = number(row, "average_bitrate");
    if (!result.bitrate)
        result.bitrate = number(row, "bitrate");
    result.channels = number(row, "audio_channels");
    result.sample_rate = number(row, "audio_sample_rate");
    result.drc = cmcp_json_bool(cmcp_json_object_get(row, "is_drc"));
    return result;
}

static int higher_rank(const YtFormat *format, const YtFormat *current)
{
    if (!current->itag)
        return 1;
    if (format->drc != current->drc)
        return format->drc < current->drc;
    if (format->channels != current->channels)
        return format->channels > current->channels;
    return format->bitrate > current->bitrate;
}

static int first_audio_itag(int itag, unsigned *seen)
{
    if (!yt_audio_itag(itag))
        return 0;
    unsigned bit = 1u << (itag - 139);
    int first = (*seen & bit) == 0;
    *seen |= bit;
    return first;
}

static YtFormat pick_format(const cmcp_json_t *list)
{
    YtFormat best = {0};
    unsigned seen = 0;
    for (size_t i = 0; i < cmcp_json_array_len(list); ++i) {
        const cmcp_json_t *row = cmcp_json_array_at(list, i);
        /* The downloader uses the first row for a requested itag. */
        if (!first_audio_itag(number(row, "itag"), &seen))
            continue;
        YtFormat candidate = read_format(row);
        if (supported_audio(&candidate) && higher_rank(&candidate, &best))
            best = candidate;
    }
    return best;
}

int yt_pick_audio(const char *json, size_t length, const char *id, YtFormat *out)
{
    if (!out || !yt_valid_id(id))
        return -1;
    cmcp_json_t *doc = metadata(json, length);
    *out = (YtFormat){0};
    if (value_is(doc, "id", id) && !cmcp_json_bool(cmcp_json_object_get(doc, "is_live")))
        *out = pick_format(cmcp_json_object_get(doc, "formats"));
    cmcp_json_free(doc);
    return out->itag ? 0 : -1;
}
