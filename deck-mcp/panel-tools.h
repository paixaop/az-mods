/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DECK_MCP_PANEL_TOOLS_H
#define DECK_MCP_PANEL_TOOLS_H
#include <stdint.h>
#define MCP_PANEL_LIMIT 16
struct mcp_panel_request {
    uint64_t token, collect_until;
    uint32_t button, hold, view, generation;
    int status;
    char key[65];
};
struct mcp_panel {
    unsigned count;
    struct mcp_panel_request records[MCP_PANEL_LIMIT];
};
#endif
