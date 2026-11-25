/* SPDX-License-Identifier: GPL-3.0-only */
#include "data-tools.h"

#include <stdlib.h>
#include <string.h>

#define ANALYSIS_ARRAY_MAX 131072u
#define VOCAL_INTERVAL_MAX 8192u

static int read_vocals(const az_analysis_v1 *api, const cmcp_json_t *args, az_vocal_snapshot_v1 *snapshot,
                       az_vocal_interval *intervals, uint32_t capacity)
{
    az_track_ref track;
    unsigned deck = mcp_input_deck(args);
    uint64_t epoch = mcp_uint64_arg(args, "loadEpoch");
    if (deck && epoch && AZ_ANALYSIS_HAS(api, vocal_intervals_loaded))
        return api->vocal_intervals_loaded(MCP_ID, (int)deck - 1, epoch, snapshot, intervals, capacity);
    if (!deck && mcp_track_arg(args, &track) && AZ_ANALYSIS_HAS(api, vocal_intervals))
        return api->vocal_intervals(MCP_ID, &track, snapshot, intervals, capacity);
    return AZ_INVALID;
}

static cmcp_json_t *vocal_page(const az_vocal_snapshot_v1 *snapshot, const az_vocal_interval *intervals,
                               uint64_t offset, uint32_t limit)
{
    cmcp_json_t *out = cmcp_json_new_object();
    cmcp_json_t *list = cmcp_json_new_array();
    uint64_t end = offset + limit < snapshot->interval_count ? offset + limit : snapshot->interval_count;
    for (uint64_t i = offset; i < end; ++i) {
        cmcp_json_t *row = cmcp_json_new_object();
        mcp_set(row, "startSeconds", cmcp_json_new_double(intervals[i].start_s));
        mcp_set(row, "stopSeconds", cmcp_json_new_double(intervals[i].stop_s));
        mcp_sdk_append(list, row);
    }
    mcp_set(out, "intervals", list);
    mcp_set(out, "count", cmcp_json_new_int(snapshot->interval_count));
    mcp_set(out, "nextOffset", end < snapshot->interval_count ? cmcp_json_new_int((long long)end) : cmcp_json_new_null());
    mcp_set(out, "receipt", mcp_clock_receipt(&snapshot->info));
    mcp_set(out, "method", cmcp_json_new_int(snapshot->method));
    mcp_set(out, "sampleRate", cmcp_json_new_int(snapshot->sample_rate));
    mcp_set(out, "hopSamples", cmcp_json_new_int(snapshot->hop_samples));
    return out;
}

static int load_intervals(const az_analysis_v1 *api, const cmcp_json_t *args,
                          az_vocal_snapshot_v1 *snapshot, az_vocal_interval **rows)
{
    int status = read_vocals(api, args, snapshot, NULL, 0);
    if (status != AZ_LIMIT || !snapshot->interval_count || snapshot->interval_count > VOCAL_INTERVAL_MAX)
        return status;
    uint32_t capacity = snapshot->interval_count;
    *rows = calloc(capacity, sizeof **rows);
    if (!*rows)
        return AZ_FAILED;
    status = read_vocals(api, args, snapshot, *rows, capacity);
    return status == AZ_OK && snapshot->interval_count > capacity ? AZ_FAILED : status;
}

static int vocals_tool(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                        cmcp_json_t **content, int *is_error)
{
    (void)is_error;
    struct mcp_runtime *runtime = user;
    az_vocal_snapshot_v1 snapshot = { .size = sizeof snapshot };
    az_vocal_interval *rows = NULL;
    int status = load_intervals(runtime->analysis, args, &snapshot, &rows);
    cmcp_json_t *out = mcp_sdk_result(runtime, status);
    uint64_t offset = az_mcp_number(args, "offset");
    if (status == AZ_OK && offset && mcp_uint64_arg(args, "revision") != snapshot.info.revision)
        mcp_set(out, "status", cmcp_json_new_int(AZ_STALE));
    else if (status == AZ_OK)
        mcp_set(out, "page", vocal_page(&snapshot, rows, offset, mcp_page_limit(args)));
    free(rows);
    return mcp_tool_result(ctx, out, content);
}

static int analysis_target(const cmcp_json_t *args, az_analysis_request_v1 *request)
{
    unsigned deck = mcp_input_deck(args);
    if (deck) {
        request->source_kind = AZ_ANALYSIS_DECK;
        request->deck = deck - 1;
        request->load_epoch = mcp_uint64_arg(args, "loadEpoch");
        return request->load_epoch ? AZ_OK : AZ_INVALID;
    }
    request->source_kind = AZ_ANALYSIS_TRACK;
    return mcp_track_arg(args, &request->track) ? AZ_OK : AZ_INVALID;
}

static int start_native_analysis(struct mcp_runtime *runtime, const cmcp_json_t *args, uint64_t *id)
{
    if (!AZ_ANALYSIS_HAS(runtime->analysis, request))
        return AZ_UNSUPPORTED;
    const char *components = az_mcp_string(args, "components");
    if (!components)
        return AZ_INVALID;
    az_analysis_request_v1 request = { .size = sizeof request, .version = AZ_ANALYSIS_REQUEST_VERSION,
        .phrase_threads = 1, .vocal_threads = 1, .stage_mode = AZ_ANALYSIS_SEQUENTIAL };
    if (strcmp(components, "vocals"))
        request.components |= AZ_DATA_PHRASES;
    if (strcmp(components, "phrases"))
        request.components |= AZ_ANALYSIS_VOCALS;
    int status = analysis_target(args, &request);
    return status == AZ_OK ? runtime->analysis->request(MCP_ID, &request, id) : status;
}

static struct mcp_analysis_record *find_analysis_record(struct mcp_runtime *runtime, const char *key)
{
    struct mcp_analysis_record *free_record = NULL;
    for (size_t i = 0; i < MCP_ANALYSIS_KEYS; ++i) {
        struct mcp_analysis_record *record = &runtime->analysis_keys[i];
        const char *prior = az_mcp_string(record->args, "key");
        if (prior && !strcmp(prior, key))
            return record;
        if (!record->id && !free_record)
            free_record = record;
    }
    return free_record;
}

static bool analysis_grant(const struct mcp_runtime *runtime, const cmcp_json_t *args)
{
    uint64_t expected = az_mcp_number(args, "armGeneration");
    return expected && expected == atomic_load(&runtime->saved_generation) &&
        az_mcp_before(az_mcp_clock(), atomic_load(&runtime->saved_edits));
}

static int analysis_retry(struct mcp_analysis_record *record, const cmcp_json_t *args, uint64_t *id)
{
    if (!cmcp_json_equal(record->args, args))
        return AZ_INVALID;
    *id = record->id;
    return AZ_OK;
}

static int retain_analysis(struct mcp_runtime *runtime, struct mcp_analysis_record *record,
                           const cmcp_json_t *args, uint64_t *id)
{
    cmcp_json_t *copy = cmcp_json_clone(args);
    if (!copy)
        return AZ_FAILED;
    int status = start_native_analysis(runtime, args, id);
    if (status == AZ_OK)
        *record = (struct mcp_analysis_record){ .id = *id, .args = copy };
    else
        cmcp_json_free(copy);
    return status;
}

static int analysis_start(struct mcp_runtime *runtime, const cmcp_json_t *args, uint64_t *id)
{
    const char *key = az_mcp_string(args, "key");
    if (!mcp_session_arg(runtime, args) || !key || !key[0] || strlen(key) >= MCP_KEY_SIZE)
        return AZ_INVALID;
    struct mcp_analysis_record *record = find_analysis_record(runtime, key);
    if (!record)
        return AZ_LIMIT;
    if (record->id)
        return analysis_retry(record, args, id);
    if (!analysis_grant(runtime, args))
        return AZ_CANCELLED;
    return retain_analysis(runtime, record, args, id);
}

static int analysis_status(const az_analysis_v1 *api, uint64_t id, cmcp_json_t *out)
{
    if (!AZ_ANALYSIS_HAS(api, status))
        return AZ_UNSUPPORTED;
    az_analysis_status_v1 state = { .size = sizeof state };
    int status = api->status(MCP_ID, id, &state);
    if (status == AZ_OK) {
        mcp_set(out, "state", cmcp_json_new_int(state.state));
        mcp_set(out, "resultStatus", cmcp_json_new_int(state.result));
        mcp_set(out, "completedComponents", cmcp_json_new_int(state.completed_components));
        mcp_set(out, "loadEpoch", mcp_id(state.load_epoch));
    }
    return status;
}

static cmcp_json_t *phrase_json(const az_analysis_phrase_v1 *phrase)
{
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "startBeat", cmcp_json_new_int(phrase->start_beat));
    mcp_set(out, "kind", cmcp_json_new_int(phrase->kind));
    mcp_set(out, "variant1", cmcp_json_new_int(phrase->variant1));
    mcp_set(out, "variant2", cmcp_json_new_int(phrase->variant2));
    mcp_set(out, "variant3", cmcp_json_new_int(phrase->variant3));
    mcp_set(out, "structure", cmcp_json_new_int(phrase->structure));
    mcp_set(out, "beat2", cmcp_json_new_int(phrase->beat2));
    mcp_set(out, "beat3", cmcp_json_new_int(phrase->beat3));
    mcp_set(out, "beat4", cmcp_json_new_int(phrase->beat4));
    mcp_set(out, "fill", cmcp_json_new_int(phrase->fill));
    mcp_set(out, "fillStartBeat", cmcp_json_new_int(phrase->fill_start_beat));
    return out;
}

struct analysis_page {
    az_analysis_result_v1 snapshot;
    az_analysis_phrase_v1 *phrases;
    az_vocal_frame_v1 *vocals;
};

static void append_result(cmcp_json_t *out, const struct analysis_page *page, uint64_t first, uint32_t limit)
{
    const az_analysis_result_v1 *result = &page->snapshot;
    cmcp_json_t *p = cmcp_json_new_array(), *v = cmcp_json_new_array();
    for (uint64_t i = first; i < first + limit && i < result->phrases.count; ++i)
        mcp_sdk_append(p, phrase_json(&page->phrases[i]));
    for (uint64_t i = first; i < first + limit && i < result->vocals.count; ++i)
        mcp_sdk_append(v, cmcp_json_new_int(page->vocals[i].activity));
    mcp_set(out, "phrases", p);
    mcp_set(out, "vocalActivity", v);
    mcp_set(out, "phraseCount", cmcp_json_new_int(result->phrases.count));
    mcp_set(out, "vocalCount", cmcp_json_new_int(result->vocals.count));
    mcp_set(out, "vocalSampleRate", cmcp_json_new_int(result->vocals.sample_rate));
    mcp_set(out, "vocalHopSamples", cmcp_json_new_int(result->vocals.hop_samples));
    mcp_set(out, "sourceRate", cmcp_json_new_int(result->source_rate));
    mcp_set(out, "sourceFrames", mcp_id(result->source_frames));
}

static bool allocate_analysis_arrays(struct analysis_page *page, uint32_t phrases, uint32_t vocals)
{
    page->phrases = calloc(phrases ? phrases : 1, sizeof *page->phrases);
    page->vocals = calloc(vocals ? vocals : 1, sizeof *page->vocals);
    return page->phrases && page->vocals;
}

static int copy_analysis_arrays(const az_analysis_v1 *api, uint64_t id, struct analysis_page *page,
                                uint32_t p, uint32_t v)
{
    if (p > ANALYSIS_ARRAY_MAX || v > ANALYSIS_ARRAY_MAX)
        return AZ_LIMIT;
    if (!allocate_analysis_arrays(page, p, v))
        return AZ_FAILED;
    int status = api->result(MCP_ID, id, &page->snapshot, page->phrases, p, page->vocals, v);
    if (status == AZ_OK && (page->snapshot.phrases.count > p || page->snapshot.vocals.count > v))
        return AZ_FAILED;
    return status;
}
static int acquire_arrays(const az_analysis_v1 *api, uint64_t id, struct analysis_page *page)
{
    int status = api->result(MCP_ID, id, &page->snapshot, NULL, 0, NULL, 0);
    if (status != AZ_LIMIT)
        return status;
    return copy_analysis_arrays(api, id, page, page->snapshot.phrases.count, page->snapshot.vocals.count);
}

static int analysis_result(const az_analysis_v1 *api, uint64_t id, const cmcp_json_t *args, cmcp_json_t *out)
{
    if (!AZ_ANALYSIS_HAS(api, result))
        return AZ_UNSUPPORTED;
    struct analysis_page page = { .snapshot = { .size = sizeof page.snapshot } };
    int status = acquire_arrays(api, id, &page);
    if (status == AZ_OK)
        append_result(out, &page, az_mcp_number(args, "offset"), mcp_page_limit(args));
    free(page.phrases);
    free(page.vocals);
    return status;
}

static int analysis_control(struct mcp_runtime *runtime, const cmcp_json_t *args,
                             const char *action, uint64_t id, cmcp_json_t *out)
{
    if (!id || !mcp_session_arg(runtime, args))
        return AZ_STALE;
    const az_analysis_v1 *api = runtime->analysis;
    if (!strcmp(action, "status"))
        return analysis_status(api, id, out);
    if (!strcmp(action, "result"))
        return analysis_result(api, id, args, out);
    if (!strcmp(action, "cancel") && AZ_ANALYSIS_HAS(api, cancel))
        return api->cancel(MCP_ID, id);
    if (!strcmp(action, "release") && AZ_ANALYSIS_HAS(api, release))
        return api->release(MCP_ID, id);
    return AZ_UNSUPPORTED;
}

static int analysis_tool(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                          cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    const char *action = az_mcp_string(args, "action");
    if (!action)
        return mcp_tool_error("An analysis action is required.", content, is_error);
    cmcp_json_t *out = mcp_session_result(runtime);
    uint64_t id = mcp_uint64_arg(args, "id");
    int status = !strcmp(action, "start") ? analysis_start(runtime, args, &id)
                                         : analysis_control(runtime, args, action, id, out);
    mcp_set(out, "status", cmcp_json_new_int(status));
    if (id)
        mcp_set(out, "id", mcp_id(id));
    return mcp_tool_result(ctx, out, content);
}

int mcp_register_analysis_tools(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    const az_analysis_v1 *api = runtime->analysis;
    const cmcp_tool_t vocals = { .name = "deck_read_vocals", .description = "Read paged vocal intervals.",
        .input_schema = MCP_SCHEMA(MCP_ARG_DECK "," MCP_ARG_TRACK "," MCP_ARG_EPOCH "," MCP_ARG_SESSION "," MCP_ARG_OFFSET "," MCP_ARG_LIMIT
            ",\"revision\":{\"type\":\"string\",\"maxLength\":20}", ""),
        .handler = vocals_tool, .userdata = runtime };
    const cmcp_tool_t analysis = { .name = "deck_analysis", .description = "Start or manage a native analysis job.",
        .input_schema = MCP_SCHEMA("\"action\":{\"enum\":[\"start\",\"status\",\"result\",\"cancel\",\"release\"]},"
            "\"components\":{\"enum\":[\"phrases\",\"vocals\",\"both\"]},"
            "\"key\":{\"type\":\"string\",\"maxLength\":64},"
            "\"armGeneration\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4294967295}," MCP_ARG_ID "," MCP_ARG_SESSION ","
            MCP_ARG_DECK "," MCP_ARG_TRACK "," MCP_ARG_EPOCH "," MCP_ARG_OFFSET "," MCP_ARG_LIMIT, "\"action\""),
        .handler = analysis_tool, .userdata = runtime };
    if ((AZ_ANALYSIS_HAS(api, vocal_intervals) || AZ_ANALYSIS_HAS(api, vocal_intervals_loaded)) &&
        cmcp_server_add_tool(server, &vocals) != CMCP_OK)
        return CMCP_EINVAL;
    return AZ_ANALYSIS_HAS(api, request) ? cmcp_server_add_tool(server, &analysis) : CMCP_OK;
}
