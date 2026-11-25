/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DECK_MCP_LIBRARY_TOOLS_H
#define DECK_MCP_LIBRARY_TOOLS_H
#include "azmod-mod.h"
#include "cmcp_json.h"
#include <stdatomic.h>
#include <stdint.h>

enum mcp_library_operation { LIST_PLAYLISTS, LIST_TRACKS, SEARCH_TRACKS, RESOLVE_TRACK,
                             PLAYLIST_ORDER, PLAYLIST_MOVE };
struct mcp_library_request {
    enum mcp_library_operation operation;
    int stick, playlist;
    uint32_t track, offset, limit;
    unsigned grant;
    az_playlist_move_v1 move;
    char query[128];
};

struct mcp_library_io {
    atomic_int phase;
    atomic_bool working;
    uint64_t id;
    uint64_t move_receipt;
    struct mcp_library_request request;
    cmcp_json_t *result;
};
#endif
