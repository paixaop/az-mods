/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DECK_MCP_DATA_TOOLS_H
#define DECK_MCP_DATA_TOOLS_H
#include "deck-mcp.h"
#include "provider/wire.h"

#define MCP_TOOL_OBJECT "{\"type\":\"object\",\"additionalProperties\":false,"
#define MCP_ARG_DECK "\"deck\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4}"
#define MCP_ARG_TRACK "\"track\":{\"type\":\"object\"}"
#define MCP_ARG_EPOCH "\"loadEpoch\":{\"type\":\"string\",\"maxLength\":20}"
#define MCP_ARG_SESSION "\"sessionId\":{\"type\":\"string\",\"maxLength\":64}"
#define MCP_ARG_ID "\"id\":{\"type\":\"string\",\"maxLength\":20}"
#define MCP_ARG_OFFSET "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":131072}"
#define MCP_ARG_LIMIT "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":128}"
#define MCP_SCHEMA(props, req) MCP_TOOL_OBJECT "\"properties\":{" props "},\"required\":[" req "]}"

uint64_t mcp_uint64_arg(const cmcp_json_t *args, const char *key);
bool mcp_track_arg(const cmcp_json_t *args, az_track_ref *out);
bool mcp_session_arg(const struct mcp_runtime *runtime, const cmcp_json_t *args);
cmcp_json_t *mcp_sdk_result(const struct mcp_runtime *runtime, int status);
bool mcp_sdk_append(cmcp_json_t *array, cmcp_json_t *value);
uint32_t mcp_page_limit(const cmcp_json_t *args);
cmcp_json_t *mcp_clock_receipt(const az_component_info *info);
cmcp_json_t *mcp_beat_json(const az_beat *beat);
cmcp_json_t *mcp_phrase_json(const az_phrase *phrase);
int mcp_register_analysis_tools(cmcp_server_t *server, struct mcp_runtime *runtime);
int mcp_register_stored_tools(cmcp_server_t *server, struct mcp_runtime *runtime);
#endif
