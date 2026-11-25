/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DECK_MCP_SETTINGS_TOOLS_H
#define DECK_MCP_SETTINGS_TOOLS_H

#include "azmod-mod.h"
#include <stdbool.h>

#define MCP_SETTINGS_MAX 96
#define MCP_SETTING_WRITES 16
#define MCP_SETTING_KEY_BYTES 65

struct mcp_setting_change {
    char key[MCP_SETTING_KEY_BYTES];
    char setting[sizeof(((az_official_setting_v1 *)0)->key)];
    int expected, value, status;
    uint64_t deadline;
};

struct mcp_settings {
    size_t count, used;
    uint64_t next_read;
    az_official_setting_v1 observed[MCP_SETTINGS_MAX];
    struct mcp_setting_change writes[MCP_SETTING_WRITES];
};

#endif
