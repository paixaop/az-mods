/* SPDX-License-Identifier: GPL-3.0-only */
#include "data-tools.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { LIB_IDLE, LIB_PENDING, LIB_READY };

struct listing {
    const struct mcp_runtime *runtime;
    const struct mcp_library_request *request;
    cmcp_json_t *rows;
    size_t matched, added;
};

static void text(cmcp_json_t *out, const char *key, const char *value)
{
    if (value)
        mcp_set(out, key, cmcp_json_new_string_n(value, mcp_utf8_prefix(value, strnlen(value, 191))));
}

static bool include_row(struct listing *listing)
{
    ++listing->matched;
    if (listing->matched <= listing->request->offset || listing->added >= listing->request->limit)
        return false;
    ++listing->added;
    return true;
}

static bool listing_finished(const struct listing *listing)
{
    return atomic_load(&listing->runtime->stopping) || listing->added >= listing->request->limit;
}

static int playlist_row(int id, int parent, const char *name, int folder, void *user)
{
    struct listing *listing = user;
    if (include_row(listing)) {
        cmcp_json_t *row = cmcp_json_new_object();
        mcp_set(row, "playlistId", cmcp_json_new_int(id));
        mcp_set(row, "parentId", cmcp_json_new_int(parent));
        mcp_set(row, "folder", cmcp_json_new_bool(folder));
        text(row, "name", name);
        mcp_sdk_append(listing->rows, row);
    }
    return listing_finished(listing);
}

static bool contains(const char *value, const char *word)
{
    return value && strcasestr(value, word);
}

static bool track_match(const struct mcp_library_request *request, const az_track *track)
{
    if (request->operation != SEARCH_TRACKS)
        return true;
    return contains(track->title, request->query) || contains(track->artist, request->query) ||
           contains(track->album, request->query) || contains(track->key, request->query);
}

static cmcp_json_t *track_row_json(const az_track *track)
{
    cmcp_json_t *row = cmcp_json_new_object();
    mcp_set(row, "trackId", mcp_id(track->id));
    text(row, "title", track->title);
    text(row, "artist", track->artist);
    text(row, "album", track->album);
    text(row, "key", track->key);
    if (isfinite(track->bpm))
        mcp_set(row, "bpm", cmcp_json_new_double(track->bpm));
    if (isfinite(track->len_s))
        mcp_set(row, "durationSeconds", cmcp_json_new_double(track->len_s));
    return row;
}

static int track_row(const az_track *track, void *user)
{
    struct listing *listing = user;
    if (track_match(listing->request, track) && include_row(listing))
        mcp_sdk_append(listing->rows, track_row_json(track));
    return listing_finished(listing);
}

static int enumerate_rows(const az_library_v1 *api, struct listing *listing)
{
    const struct mcp_library_request *request = listing->request;
    if (request->operation == LIST_PLAYLISTS)
        return AZ_LIB_HAS(api, lib_playlists)
            ? api->lib_playlists(MCP_ID, request->stick, playlist_row, listing) : AZ_UNSUPPORTED;
    return AZ_LIB_HAS(api, lib_tracks)
        ? api->lib_tracks(MCP_ID, request->stick, request->playlist, track_row, listing) : AZ_UNSUPPORTED;
}

static cmcp_json_t *list_rows(struct mcp_runtime *runtime, const struct mcp_library_request *request)
{
    struct listing listing = { .request = request, .runtime = runtime, .rows = cmcp_json_new_array() };
    int status = enumerate_rows(runtime->library, &listing);
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "status", cmcp_json_new_int(status >= 0 ? AZ_OK : status));
    mcp_set(out, "sourceTruncated", cmcp_json_new_bool(status == 1));
    mcp_set(out, "rows", listing.rows);
    mcp_set(out, "nextOffset", listing.added == request->limit
        ? cmcp_json_new_int((long long)listing.matched) : cmcp_json_new_null());
    return out;
}

static cmcp_json_t *resolve_track(struct mcp_runtime *runtime, const struct mcp_library_request *request)
{
    az_track_ref track = { .size = sizeof track, .version = AZ_DATA_VERSION };
    int status = AZ_LIB_HAS(runtime->library, track_resolve)
        ? runtime->library->track_resolve(MCP_ID, request->stick, request->track, &track)
        : AZ_UNSUPPORTED;
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "status", cmcp_json_new_int(status));
    if (status == AZ_OK) {
        cmcp_json_t *ref = cmcp_json_new_object();
        mcp_set(ref, "sourceId", mcp_id(track.source_id));
        mcp_set(ref, "sourceGeneration", mcp_id(track.source_generation));
        mcp_set(ref, "libraryId", mcp_id(track.library_id));
        mcp_set(ref, "trackId", mcp_id(track.track_id));
        mcp_set(out, "track", ref);
    }
    return out;
}

static void playlist_entries(cmcp_json_t *out, const az_playlist_order_v1 *order,
                             const az_playlist_entry_v1 *entries, uint32_t offset)
{
    mcp_set(out, "revision", mcp_id(order->revision));
    mcp_set(out, "sourceGeneration", mcp_id(order->source_generation));
    mcp_set(out, "libraryId", mcp_id(order->library_id));
    mcp_set(out, "total", cmcp_json_new_int(order->total));
    uint64_t next = (uint64_t)offset + order->returned;
    mcp_set(out, "nextOffset", next < order->total ? cmcp_json_new_int((long long)next) : cmcp_json_new_null());
    cmcp_json_t *rows = cmcp_json_new_array();
    for (uint32_t i = 0; i < order->returned; ++i) {
        cmcp_json_t *row = cmcp_json_new_object();
        mcp_set(row, "entryId", mcp_id(entries[i].entry_id));
        mcp_set(row, "trackId", mcp_id(entries[i].track_id));
        mcp_set(row, "position", cmcp_json_new_int(entries[i].position));
        mcp_sdk_append(rows, row);
    }
    mcp_set(out, "rows", rows);
}

static bool playlist_page_current(const az_playlist_move_v1 *expected, const az_playlist_order_v1 *order)
{
    return (!expected->revision || expected->revision == order->revision) &&
           (!expected->source_generation || expected->source_generation == order->source_generation) &&
           (!expected->library_id || expected->library_id == order->library_id);
}

static cmcp_json_t *read_playlist_order(struct mcp_runtime *runtime, const struct mcp_library_request *request)
{
    az_playlist_order_v1 order = { .size = sizeof order, .version = AZ_DATA_VERSION };
    az_playlist_entry_v1 entries[AZ_PLAYLIST_PAGE_LIMIT];
    int rc = runtime->library->playlist_order(MCP_ID, request->stick, (uint32_t)request->playlist,
                                              request->offset, entries, request->limit, &order);
    if (rc == AZ_OK && !playlist_page_current(&request->move, &order))
        rc = AZ_STALE;
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "status", cmcp_json_new_int(rc));
    if (rc == AZ_OK)
        playlist_entries(out, &order, entries, request->offset);
    return out;
}

static unsigned playlist_grant(const struct mcp_runtime *runtime)
{
    const struct mcp_intent edit = { .flags = AZ_COMMAND_PERSIST };
    return mcp_command_grant(runtime, &edit);
}

static cmcp_json_t *move_result(int rc, const az_playlist_move_result_v1 *result)
{
    static const char *const outcomes[] = { "notMoved", "moved", "uncertain" };
    static const char *const pdb[] = { "notAttempted", "current", "failed" };
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "status", cmcp_json_new_int(rc == AZ_OK ? result->status : rc));
    if (rc == AZ_OK) {
        mcp_set(out, "outcome", cmcp_json_new_string(outcomes[result->outcome]));
        mcp_set(out, "deviceLibrary", cmcp_json_new_string(pdb[result->device_library]));
    }
    return out;
}

static cmcp_json_t *move_playlist_track(struct mcp_runtime *runtime)
{
    struct mcp_library_io *io = &runtime->library_io;
    const struct mcp_library_request *request = &io->request;
    az_playlist_move_result_v1 result = { .size = sizeof result, .version = AZ_DATA_VERSION };
    int rc;
    if (!io->move_receipt) {
        if (request->grant != playlist_grant(runtime))
            return move_result(AZ_CANCELLED, &result);
        rc = runtime->library->playlist_move(MCP_ID, &request->move, &io->move_receipt);
    } else {
        rc = runtime->library->playlist_move_status(MCP_ID, io->move_receipt, &result);
    }
    return rc == AZ_PENDING ? NULL : move_result(rc, &result);
}

static cmcp_json_t *library_result(struct mcp_runtime *runtime, const struct mcp_library_request *request)
{
    switch (request->operation) {
    case RESOLVE_TRACK: return resolve_track(runtime, request);
    case PLAYLIST_ORDER: return read_playlist_order(runtime, request);
    case PLAYLIST_MOVE: return move_playlist_track(runtime);
    default: return list_rows(runtime, request);
    }
}

static void library_step(struct mcp_runtime *runtime)
{
    struct mcp_library_io *io = &runtime->library_io;
    if (atomic_load(&io->phase) != LIB_PENDING)
        return;
    const struct mcp_library_request *request = &io->request;
    io->result = library_result(runtime, request);
    if (io->result)
        atomic_store(&io->phase, LIB_READY);
}

static void *library_worker(void *user)
{
    struct mcp_runtime *runtime = user;
    const struct timespec sleep = { .tv_nsec = 20000000 };
    while (!atomic_load(&runtime->stopping)) {
        library_step(runtime);
        nanosleep(&sleep, NULL);
    }
    atomic_store(&runtime->library_io.working, false);
    return NULL;
}

bool mcp_start_library_io(struct mcp_runtime *runtime)
{
    const az_library_v1 *api = runtime->library;
    struct mcp_library_io *io = &runtime->library_io;
    if (!AZ_LIB_HAS(api, lib_playlists) && !AZ_LIB_HAS(api, lib_tracks) && !AZ_LIB_HAS(api, track_resolve))
        return false;
    if (atomic_exchange(&io->working, true))
        return false;
    cmcp_json_free(io->result);
    io->result = NULL;
    io->id = 0;
    atomic_store(&io->phase, LIB_IDLE);
    if (runtime->core->thread_start(MCP_ID, "deck-library", library_worker, runtime) == 0)
        return true;
    atomic_store(&io->working, false);
    return false;
}

static int operation_name(const char *name)
{
    const char *const actions[] = { "listPlaylists", "listTracks", "searchUsb", "resolveTrack" };
    for (size_t i = 0; i < sizeof actions / sizeof actions[0]; ++i)
        if (!strcmp(actions[i], name))
            return (int)i;
    return -1;
}

static bool request_details(struct mcp_library_request *request, const cmcp_json_t *args)
{
    switch (request->operation) {
    case SEARCH_TRACKS: {
        const char *query = az_mcp_string(args, "query");
        if (!query || !query[0] || strlen(query) >= sizeof request->query)
            return false;
        snprintf(request->query, sizeof request->query, "%s", query);
        return true;
    }
    case RESOLVE_TRACK: {
        uint64_t track = mcp_uint64_arg(args, "trackId");
        if (!track || track > UINT32_MAX)
            return false;
        request->track = (uint32_t)track;
        return true;
    }
    default: return true;
    }
}

static bool parse_request(const char *action, const cmcp_json_t *args, struct mcp_library_request *out)
{
    uint64_t stick = az_mcp_number(args, "usb");
    uint64_t playlist = az_mcp_number(args, "playlistId");
    int operation = operation_name(action);
    if (operation < 0 || stick < 1 || stick > 2 || playlist > INT_MAX)
        return false;
    *out = (struct mcp_library_request){ .operation = operation, .stick = (int)stick - 1,
        .playlist = (int)playlist, .offset = (uint32_t)az_mcp_number(args, "offset"),
        .limit = mcp_page_limit(args) };
    return request_details(out, args);
}

static int submit_library(struct mcp_library_io *io, const struct mcp_library_request *request)
{
    if (atomic_load(&io->phase) == LIB_PENDING)
        return AZ_BUSY;
    if (io->id == UINT64_MAX)
        return AZ_LIMIT;
    cmcp_json_free(io->result);
    io->result = NULL;
    io->move_receipt = 0;
    io->request = *request;
    ++io->id;
    atomic_store(&io->phase, LIB_PENDING);
    return AZ_PENDING;
}

static int queue_library(struct mcp_library_io *io, const char *action, const cmcp_json_t *args)
{
    struct mcp_library_request request;
    if (!parse_request(action, args, &request))
        return AZ_INVALID;
    return submit_library(io, &request);
}

static int library_status(struct mcp_runtime *runtime, const cmcp_json_t *args, cmcp_json_t *out)
{
    const struct mcp_library_io *io = &runtime->library_io;
    uint64_t id = mcp_uint64_arg(args, "id");
    if (!id || id != io->id || !mcp_session_arg(runtime, args))
        return AZ_STALE;
    if (atomic_load(&io->phase) != LIB_READY)
        return AZ_PENDING;
    mcp_set(out, "result", cmcp_json_clone(io->result));
    return AZ_OK;
}

static int library_tool(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                         cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    const char *action = az_mcp_string(args, "action");
    if (!action)
        return mcp_tool_error("A library action is required.", content, is_error);
    cmcp_json_t *out = mcp_session_result(runtime);
    int status = !strcmp(action, "status") ? library_status(runtime, args, out)
                                          : queue_library(&runtime->library_io, action, args);
    mcp_set(out, "status", cmcp_json_new_int(status));
    if (status == AZ_PENDING || status == AZ_OK)
        mcp_set(out, "id", mcp_id(runtime->library_io.id));
    return mcp_tool_result(ctx, out, content);
}

static int queue_playlist_move(struct mcp_runtime *runtime, const cmcp_json_t *args,
                                struct mcp_library_request *request)
{
    if (!mcp_session_arg(runtime, args))
        return AZ_INVALID;
    request->grant = (unsigned)az_mcp_number(args, "armGeneration");
    if (!request->grant || request->grant != playlist_grant(runtime))
        return AZ_CANCELLED;
    az_playlist_move_v1 *move = &request->move;
    move->size = sizeof *move;
    move->version = AZ_DATA_VERSION;
    move->stick = (uint32_t)request->stick;
    move->playlist_id = (uint32_t)request->playlist;
    move->from = (uint32_t)az_mcp_number(args, "fromPosition");
    move->to = (uint32_t)az_mcp_number(args, "toPosition");
    move->entry_id = mcp_uint64_arg(args, "entryId");
    return submit_library(&runtime->library_io, request);
}

static int queue_playlist(struct mcp_runtime *runtime, const char *action, const cmcp_json_t *args)
{
    struct mcp_library_request request;
    if (!parse_request("listTracks", args, &request) || request.playlist <= 0)
        return AZ_INVALID;
    request.move.revision = mcp_uint64_arg(args, "revision");
    request.move.source_generation = mcp_uint64_arg(args, "sourceGeneration");
    request.move.library_id = mcp_uint64_arg(args, "libraryId");
    if (!strcmp(action, "getOrder")) {
        request.operation = PLAYLIST_ORDER;
        return submit_library(&runtime->library_io, &request);
    }
    request.operation = PLAYLIST_MOVE;
    return queue_playlist_move(runtime, args, &request);
}

static int playlist_edit_tool(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                               cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    const char *action = az_mcp_string(args, "action");
    if (!action)
        return mcp_tool_error("A playlist action is required.", content, is_error);
    cmcp_json_t *out = mcp_session_result(runtime);
    int rc = !strcmp(action, "status") ? library_status(runtime, args, out)
                                        : queue_playlist(runtime, action, args);
    mcp_set(out, "status", cmcp_json_new_int(rc));
    if (rc == AZ_PENDING || rc == AZ_OK)
        mcp_set(out, "id", mcp_id(runtime->library_io.id));
    return mcp_tool_result(ctx, out, content);
}

static int register_playlist_editor(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    const az_library_v1 *api = runtime->library;
    if (!AZ_LIB_HAS(api, playlist_order) || !AZ_LIB_HAS(api, playlist_move) ||
        !AZ_LIB_HAS(api, playlist_move_status))
        return CMCP_OK;
    const cmcp_tool_t edit = { .name = "deck_library_edit",
        .description = "Read USB playlist order, move one exact entry to a final one-based position, or poll the operation.",
        .input_schema = MCP_SCHEMA("\"action\":{\"enum\":[\"getOrder\",\"moveTrack\",\"status\"]},"
            "\"usb\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2},"
            "\"playlistId\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2147483647},"
            "\"fromPosition\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":20000},"
            "\"toPosition\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":20000},"
            "\"revision\":{\"type\":\"string\",\"maxLength\":20},"
            "\"entryId\":{\"type\":\"string\",\"maxLength\":20},"
            "\"sourceGeneration\":{\"type\":\"string\",\"maxLength\":20},"
            "\"libraryId\":{\"type\":\"string\",\"maxLength\":20},"
            "\"armGeneration\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4294967295}," MCP_ARG_OFFSET ","
            MCP_ARG_LIMIT "," MCP_ARG_ID "," MCP_ARG_SESSION, "\"action\""),
        .handler = playlist_edit_tool, .userdata = runtime };
    return cmcp_server_add_tool(server, &edit);
}

int mcp_register_library_tools(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    if (!atomic_load(&runtime->library_io.working))
        return CMCP_OK;
    const cmcp_tool_t tool = { .name = "deck_library",
        .description = "Queue a USB library lookup or read the result of the most recent lookup.",
        .input_schema = MCP_SCHEMA("\"action\":{\"enum\":[\"listPlaylists\",\"listTracks\",\"searchUsb\",\"resolveTrack\",\"status\"]},"
            "\"usb\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2},\"playlistId\":{\"type\":\"integer\","
            "\"minimum\":0,\"maximum\":2147483647},\"query\":{\"type\":\"string\",\"maxLength\":127},"
            "\"trackId\":{\"type\":\"string\",\"maxLength\":10}," MCP_ARG_OFFSET "," MCP_ARG_LIMIT "," MCP_ARG_ID "," MCP_ARG_SESSION,
            "\"action\""),
        .handler = library_tool, .userdata = runtime };
    int rc = cmcp_server_add_tool(server, &tool);
    return rc == CMCP_OK ? register_playlist_editor(server, runtime) : rc;
}
