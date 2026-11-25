/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"

/* Reuse the pinned server dispatcher on the Libevent worker. The upstream
 * run loop couples its transport to asynchronous replies and owns signals,
 * neither of which is needed for these synchronous, bounded handlers. */
#ifdef __APPLE__
/* macOS lacks monotonic pthread condition variables. The upstream run loop
 * and its timed watchdog are unused in these host tests. */
#define pthread_condattr_setclock(attr, clock) ((void)(attr), (void)(clock), 0)
#endif
#include "server.c"

static void refresh_catalog_hint(const cmcp_rpc_message_t *request, cmcp_rpc_message_t *response)
{
    if (!request->method || strcmp(request->method, "tools/list") || !response->result)
        return;
    /* Registration can change between calls. Prevent a modern HTTP client
     * from holding a stale cache entry after a provider disconnects. */
    if (cmcp_json_object_get(response->result, "ttlMs"))
        mcp_set(response->result, "ttlMs", cmcp_json_new_int(0));
}

int mcp_exchange(cmcp_server_t *server, const cmcp_rpc_message_t *request, char **wire)
{
    cmcp_rpc_message_t response;
    cmcp_handler_ctx_t context;
    hctx_init(&context, server, request->params, 0);
    /* The HTTP endpoint issues no legacy session ID. Handshakes are
     * independent and legacy calls can arrive on any authenticated connection. */
    server->state = request->method && strcmp(request->method, "initialize") == 0
                        ? SS_UNINIT : SS_READY;
    server_handle_message(server, request, &response, &context);
    refresh_catalog_hint(request, &response);
    bool reply = response.kind == CMCP_MSG_RESPONSE;
    *wire = reply ? cmcp_rpc_emit(&response) : NULL;
    cmcp_rpc_message_clear(&response);
    return reply && !*wire ? 500 : *wire ? 200 : 202;
}

/* Called only on the gateway worker between dispatches. cMCP owns and
 * deep-copies each descriptor at registration. */
void mcp_remove_tool(cmcp_server_t *server, const char *name)
{
    server_tool_t *tool = tool_find(server, name);
    if (!tool)
        return;
    size_t index = (size_t)(tool - server->tools);
    tool_clear(tool);
    --server->n_tools;
    memmove(tool, tool + 1, (server->n_tools - index) * sizeof *tool);
}
