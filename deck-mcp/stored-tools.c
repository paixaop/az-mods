/* SPDX-License-Identifier: GPL-3.0-only */
#include "data-tools.h"

#include <openssl/evp.h>
#include <string.h>

#define MAX_WAVE_COLUMNS 512

static bool source_ref(struct mcp_runtime *runtime, const cmcp_json_t *args, az_track_ref *track)
{
    unsigned deck = mcp_input_deck(args);
    if (!deck)
        return mcp_track_arg(args, track);
    uint64_t load = mcp_uint64_arg(args, "loadEpoch");
    pthread_mutex_lock(&runtime->lock);
    const struct mcp_deck *snapshot = &runtime->published[deck - 1];
    bool valid = load && snapshot->load_epoch == load && snapshot->track.size;
    if (valid)
        *track = snapshot->track;
    pthread_mutex_unlock(&runtime->lock);
    return valid;
}

static const struct {
    const char *name;
    uint32_t component;
} components[] = {
    { "metadata", AZ_DATA_METADATA }, { "cues", AZ_DATA_CUES },
    { "grid", AZ_DATA_GRID }, { "phrases", AZ_DATA_PHRASES }, { "overview", AZ_DATA_WAVEFORM }
};

static uint32_t component_arg(const cmcp_json_t *args)
{
    const char *name = az_mcp_string(args, "component");
    for (size_t i = 0; name && i < sizeof components / sizeof components[0]; ++i)
        if (!strcmp(components[i].name, name))
            return components[i].component;
    return 0;
}

static const az_component_info *stored_receipt(const az_track_snapshot *snapshot, uint32_t component)
{
    switch (component) {
    case AZ_DATA_METADATA: return &snapshot->metadata_info;
    case AZ_DATA_CUES: return &snapshot->cues_info;
    case AZ_DATA_GRID: return &snapshot->grid_info;
    case AZ_DATA_PHRASES: return &snapshot->phrases_info;
    case AZ_DATA_WAVEFORM: return &snapshot->waveform_info;
    }
    return NULL;
}

static void valid_text(cmcp_json_t *out, const char *key, const char *str, size_t length)
{
    size_t used = mcp_utf8_prefix(str, strnlen(str, length));
    mcp_set(out, key, cmcp_json_new_string_n(str, used));
}

static void metadata_json(cmcp_json_t *out, const az_track_metadata *metadata)
{
    if (metadata->valid & AZ_META_TITLE)
        valid_text(out, "title", metadata->title, sizeof metadata->title);
    if (metadata->valid & AZ_META_ARTIST)
        valid_text(out, "artist", metadata->artist, sizeof metadata->artist);
    if (metadata->valid & AZ_META_ALBUM)
        valid_text(out, "album", metadata->album, sizeof metadata->album);
    if (metadata->valid & AZ_META_KEY)
        valid_text(out, "key", metadata->key, sizeof metadata->key);
    if (metadata->valid & AZ_META_DURATION)
        mcp_set(out, "durationSeconds", cmcp_json_new_double(metadata->duration_s));
    if (metadata->valid & AZ_META_TEMPO)
        mcp_set(out, "trackBpm", cmcp_json_new_double(metadata->original_bpm));
}

cmcp_json_t *mcp_beat_json(const az_beat *beat)
{
    cmcp_json_t *row = cmcp_json_new_object();
    mcp_set(row, "timeSeconds", cmcp_json_new_double(beat->time_s));
    mcp_set(row, "bpm", cmcp_json_new_double(beat->bpm));
    mcp_set(row, "beatInBar", cmcp_json_new_int(beat->beat_in_bar));
    return row;
}

cmcp_json_t *mcp_phrase_json(const az_phrase *phrase)
{
    cmcp_json_t *row = cmcp_json_new_object();
    mcp_set(row, "startBeatIndex", cmcp_json_new_int(phrase->start_beat));
    mcp_set(row, "endBeatIndex", cmcp_json_new_int(phrase->end_beat));
    mcp_set(row, "rawKind", cmcp_json_new_int(phrase->raw_kind));
    mcp_set(row, "rawMood", cmcp_json_new_int(phrase->raw_mood));
    mcp_set(row, "category", cmcp_json_new_int(phrase->category));
    return row;
}

static cmcp_json_t *base64(const uint8_t *bytes, uint32_t count)
{
    char encoded[4 * ((MAX_WAVE_COLUMNS * 6 + 2) / 3) + 1];
    if (!count)
        return cmcp_json_new_string("");
    if (!bytes || count > MAX_WAVE_COLUMNS * 6)
        return cmcp_json_new_null();
    int length = EVP_EncodeBlock((unsigned char *)encoded, bytes, (int)count);
    return length >= 0 ? cmcp_json_new_string_n(encoded, (size_t)length) : cmcp_json_new_null();
}

static uint32_t stored_count(const az_track_snapshot *snap, uint32_t component)
{
    switch (component) {
    case AZ_DATA_CUES: return snap->cues ? snap->cue_count : 0;
    case AZ_DATA_GRID: return snap->beats ? snap->beat_count : 0;
    case AZ_DATA_PHRASES: return snap->phrases ? snap->phrase_count : 0;
    case AZ_DATA_WAVEFORM: return snap->waveform.samples ? snap->waveform.sample_count : 0;
    }
    return 0;
}

static cmcp_json_t *stored_row(const az_track_snapshot *snap, uint32_t component, uint32_t index)
{
    switch (component) {
    case AZ_DATA_CUES: return mcp_cue_entry_json(&snap->cues[index]);
    case AZ_DATA_GRID: return mcp_beat_json(&snap->beats[index]);
    case AZ_DATA_PHRASES: return mcp_phrase_json(&snap->phrases[index]);
    }
    return NULL;
}

static void stored_overview(cmcp_json_t *out, const az_waveform *wave, uint32_t offset, uint32_t count)
{
    mcp_set(out, "representation", cmcp_json_new_int(wave->representation));
    mcp_set(out, "startSeconds", cmcp_json_new_double(wave->start_s));
    mcp_set(out, "secondsPerColumn", cmcp_json_new_double(wave->seconds_per_sample));
    mcp_set(out, "base64", base64(count ? wave->samples + offset : NULL, count));
}

static void stored_page(cmcp_json_t *out, const az_track_snapshot *snap, uint32_t component,
                        uint32_t offset, uint32_t limit)
{
    uint32_t count = stored_count(snap, component);
    uint32_t end = offset + limit < count ? offset + limit : count;
    mcp_set(out, "count", cmcp_json_new_int(count));
    mcp_set(out, "nextOffset", end < count ? cmcp_json_new_int(end) : cmcp_json_new_null());
    if (component == AZ_DATA_WAVEFORM) {
        stored_overview(out, &snap->waveform, offset, offset < end ? end - offset : 0);
        return;
    }
    cmcp_json_t *rows = cmcp_json_new_array();
    for (uint32_t i = offset; i < end; ++i)
        mcp_sdk_append(rows, stored_row(snap, component, i));
    mcp_set(out, "rows", rows);
}

static int stored_copy(cmcp_json_t *out, const az_track_snapshot *snap, const cmcp_json_t *args)
{
    uint32_t component = component_arg(args);
    const az_component_info *receipt = stored_receipt(snap, component);
    if (!receipt)
        return AZ_INVALID;
    mcp_set(out, "receipt", mcp_clock_receipt(receipt));
    if (receipt->availability != AZ_AVAIL_READY)
        return AZ_EMPTY;
    uint32_t offset = (uint32_t)az_mcp_number(args, "offset");
    if (offset && mcp_uint64_arg(args, "revision") != receipt->revision)
        return AZ_STALE;
    if (component == AZ_DATA_METADATA)
        metadata_json(out, &snap->metadata);
    else
        stored_page(out, snap, component, offset, mcp_page_limit(args));
    return AZ_OK;
}

static int stored_read(struct mcp_runtime *runtime, const cmcp_json_t *args, uint64_t id, cmcp_json_t *out)
{
    const az_track_snapshot *snap = NULL;
    int status = runtime->library->track_acquire(MCP_ID, id, &snap);
    return status == AZ_OK && snap ? stored_copy(out, snap, args) : status;
}

static int stored_request(struct mcp_runtime *runtime, const cmcp_json_t *args, uint64_t *id)
{
    az_track_ref track;
    uint32_t component = component_arg(args);
    if (!component || !source_ref(runtime, args, &track))
        return AZ_INVALID;
    return runtime->library->track_request(MCP_ID, &track, component, id);
}

static int waveform_request(struct mcp_runtime *runtime, const cmcp_json_t *args, uint64_t *id)
{
    az_track_ref track;
    uint64_t encoding = az_mcp_number(args, "encoding");
    uint64_t columns = az_mcp_number(args, "columns");
    if (!encoding || !columns || !source_ref(runtime, args, &track))
        return AZ_INVALID;
    const az_wave_query query = { .size = sizeof query, .version = AZ_DATA_VERSION,
        .encoding = (uint32_t)encoding, .column_count = (uint32_t)columns,
        .first_column = (uint32_t)az_mcp_number(args, "firstColumn") };
    return runtime->library->waveform_request(MCP_ID, &track, &query, id);
}

static int wave_copy(cmcp_json_t *out, const az_wave_snapshot *wave)
{
    mcp_set(out, "receipt", mcp_clock_receipt(&wave->info));
    mcp_set(out, "encoding", cmcp_json_new_int(wave->encoding));
    mcp_set(out, "firstColumn", cmcp_json_new_int(wave->first_column));
    mcp_set(out, "columns", cmcp_json_new_int(wave->column_count));
    mcp_set(out, "totalColumns", cmcp_json_new_int(wave->total_columns));
    mcp_set(out, "bytesPerColumn", cmcp_json_new_int(wave->bytes_per_column));
    mcp_set(out, "startSeconds", cmcp_json_new_double(wave->start_s));
    mcp_set(out, "secondsPerColumn", cmcp_json_new_double(wave->seconds_per_column));
    mcp_set(out, "base64", base64(wave->columns, wave->data_bytes));
    return AZ_OK;
}

static int wave_read(struct mcp_runtime *runtime, uint64_t id, cmcp_json_t *out)
{
    const az_wave_snapshot *wave = NULL;
    int status = runtime->library->waveform_acquire(MCP_ID, id, &wave);
    return status == AZ_OK && wave ? wave_copy(out, wave) : status;
}

static int ticket_control(struct mcp_runtime *runtime, const char *action, uint64_t id)
{
    if (!strcmp(action, "release"))
        return runtime->library->track_release(MCP_ID, id);
    if (!strcmp(action, "cancel"))
        return AZ_LIB_HAS(runtime->library, track_cancel)
            ? runtime->library->track_cancel(MCP_ID, id) : AZ_UNSUPPORTED;
    return AZ_INVALID;
}

static int ticket_action(struct mcp_runtime *runtime, const cmcp_json_t *args,
                         const char *action, uint64_t *id, cmcp_json_t *out, bool wave)
{
    if (!strcmp(action, "request"))
        return wave ? waveform_request(runtime, args, id) : stored_request(runtime, args, id);
    if (!*id || !mcp_session_arg(runtime, args))
        return AZ_STALE;
    if (!strcmp(action, "read"))
        return wave ? wave_read(runtime, *id, out) : stored_read(runtime, args, *id, out);
    return ticket_control(runtime, action, *id);
}

static int ticket_tool(const cmcp_json_t *args, struct mcp_runtime *runtime, bool wave,
                       cmcp_handler_ctx_t *ctx, cmcp_json_t **content, int *is_error)
{
    const char *action = az_mcp_string(args, "action");
    if (!action)
        return mcp_tool_error("A ticket action is required.", content, is_error);
    cmcp_json_t *out = mcp_session_result(runtime);
    uint64_t id = mcp_uint64_arg(args, "id");
    int status = ticket_action(runtime, args, action, &id, out, wave);
    mcp_set(out, "status", cmcp_json_new_int(status));
    if (id)
        mcp_set(out, "id", mcp_id(id));
    return mcp_tool_result(ctx, out, content);
}

static int read_track(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                       cmcp_json_t **content, int *is_error)
{
    return ticket_tool(args, user, false, ctx, content, is_error);
}

static int read_waveform(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                          cmcp_json_t **content, int *is_error)
{
    return ticket_tool(args, user, true, ctx, content, is_error);
}

#define TICKET_FIELDS "\"action\":{\"enum\":[\"request\",\"read\",\"release\",\"cancel\"]}," MCP_ARG_ID "," \
    MCP_ARG_SESSION "," MCP_ARG_DECK "," MCP_ARG_TRACK "," MCP_ARG_EPOCH

int mcp_register_stored_tools(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    const az_library_v1 *api = runtime->library;
    if (!AZ_LIB_HAS(api, track_release))
        return CMCP_OK;
    const cmcp_tool_t track = { .name = "deck_read_track_analysis",
        .description = "Read stored metadata, cues, grid, phrases or overview through a USB track ticket.",
        .input_schema = MCP_SCHEMA(TICKET_FIELDS ",\"component\":{\"enum\":[\"metadata\",\"cues\",\"grid\",\"phrases\",\"overview\"]},"
            MCP_ARG_OFFSET "," MCP_ARG_LIMIT ",\"revision\":{\"type\":\"string\",\"maxLength\":20}", "\"action\""),
        .handler = read_track, .userdata = runtime };
    const cmcp_tool_t wave = { .name = "deck_read_waveform",
        .description = "Request a bounded encoded USB waveform window, then read its base64 columns.",
        .input_schema = MCP_SCHEMA(TICKET_FIELDS ",\"encoding\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8},"
            "\"firstColumn\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000000},"
            "\"columns\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":512}", "\"action\""),
        .handler = read_waveform, .userdata = runtime };
    if (AZ_LIB_HAS(api, track_request) && AZ_LIB_HAS(api, track_acquire) && cmcp_server_add_tool(server, &track))
        return CMCP_EINVAL;
    if (AZ_LIB_HAS(api, waveform_request) && AZ_LIB_HAS(api, waveform_acquire))
        return cmcp_server_add_tool(server, &wave);
    return CMCP_OK;
}
