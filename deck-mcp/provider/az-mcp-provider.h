/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_MCP_PROVIDER_H
#define AZ_MCP_PROVIDER_H

#include "azmod-mod.h"
#include "cmcp_json.h"

/* Link provider.c, wire.c and the pinned cMCP json.c into the owning mod.
 * Descriptor tables and userdata live for the lifetime of that mod. */
#define AZ_MCP_PROVIDER_VERSION 1
#define AZ_MCP_PROVIDER_MAX_TOOLS 24
#define AZ_MCP_PROVIDER_FRAME (160u * 1024u)
enum az_mcp_grant { AZ_MCP_READ, AZ_MCP_LIVE, AZ_MCP_SAVED };

typedef struct az_mcp_provider az_mcp_provider;
typedef struct az_mcp_tool_v1 {
    size_t size;
    const char *name, *title, *description, *input_schema, *output_schema;
    enum az_mcp_grant grant;
    /* UI thread. Return AZ_OK with a result, another AZ_* code on failure,
     * or AZ_PENDING and complete later. The worker releases result trees.
     * The input tree stays borrowed until the callback returns or, for
     * AZ_PENDING, until az_mcp_provider_complete accepts the result. */
    int (*invoke)(const cmcp_json_t *args, uint64_t call_id, uint64_t deadline_ns,
                  cmcp_json_t **result, void *user);
    void *user;
} az_mcp_tool_v1;

int az_mcp_register_tools(const az_core_v1 *core, const char *mod_id,
                          const az_mcp_tool_v1 *tools, size_t count,
                          az_mcp_provider **out);
void az_mcp_provider_tick(az_mcp_provider *provider);
int az_mcp_provider_complete(az_mcp_provider *provider, uint64_t id,
                              int status, cmcp_json_t *result);
void az_mcp_provider_stop(az_mcp_provider *provider);

#endif
