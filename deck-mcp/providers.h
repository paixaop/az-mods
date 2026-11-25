/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DECK_MCP_PROVIDER_SERVER_H
#define DECK_MCP_PROVIDER_SERVER_H
#include "http.h"

struct event_base;
struct mcp_providers;
struct mcp_providers *mcp_providers_start(struct event_base *base, cmcp_server_t *server,
                                          struct mcp_runtime *runtime, const struct mcp_config *config);
void mcp_providers_sweep(struct mcp_providers *providers);
void mcp_providers_stop(struct mcp_providers *providers);
void mcp_provider_controls_init(struct mcp_runtime *runtime);
void mcp_saved_arm(struct mcp_runtime *runtime);
void mcp_saved_disarm(struct mcp_runtime *runtime);
#endif
