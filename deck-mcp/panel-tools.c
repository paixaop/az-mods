/* SPDX-License-Identifier: GPL-3.0-only */
#include "data-tools.h"

#include <stdio.h>
#include <string.h>

static uint32_t button_arg(const cmcp_json_t *args)
{
    static const struct { const char *name; uint32_t button; } names[] = {
        { "source", AZ_PANEL_SOURCE }, { "browse", AZ_PANEL_BROWSE }, { "back", AZ_PANEL_BACK },
        { "menu", AZ_PANEL_MENU }, { "search", AZ_PANEL_SEARCH }, { "deck1", AZ_PANEL_DECK_1 },
        { "deck2", AZ_PANEL_DECK_2 }, { "deck3", AZ_PANEL_DECK_3 }, { "deck4", AZ_PANEL_DECK_4 }
    };
    const char *name = az_mcp_string(args, "button");
    for (size_t i = 0; name && i < sizeof names / sizeof names[0]; ++i)
        if (!strcmp(names[i].name, name))
            return names[i].button;
    return 0;
}

static int view_arg(const cmcp_json_t *args)
{
    const char *const views[] = { "other", "source", "browse", "search", "deck", "menu" };
    const char *name = az_mcp_string(args, "expectedView");
    for (size_t i = 0; name && i < sizeof views / sizeof views[0]; ++i)
        if (!strcmp(views[i], name))
            return (int)i;
    return -1;
}

static int parse_panel(const cmcp_json_t *args, struct mcp_panel_request *request)
{
    int view = view_arg(args);
    uint64_t hold = az_mcp_number(args, "holdMs");
    uint64_t generation = az_mcp_number(args, "armGeneration");
    uint32_t button = button_arg(args);
    if (view < 0 || !button || hold < 40 || hold > 3000 || !generation || generation > UINT32_MAX)
        return AZ_INVALID;
    request->button = button;
    request->hold = (uint32_t)hold;
    request->view = (uint32_t)view;
    request->generation = (uint32_t)generation;
    return AZ_OK;
}

static struct mcp_panel_request *panel_find(struct mcp_panel *panel, const char *key)
{
    for (unsigned i = 0; i < panel->count; ++i)
        if (!strcmp(panel->records[i].key, key))
            return &panel->records[i];
    return NULL;
}

static bool same_panel(const struct mcp_panel_request *a, const struct mcp_panel_request *b)
{
    return a->button == b->button && a->hold == b->hold &&
           a->view == b->view && a->generation == b->generation;
}

void mcp_panel_reap(struct mcp_panel *panel, uint64_t now)
{
    unsigned kept = 0;
    for (unsigned i = 0; i < panel->count; ++i)
        if (!mcp_receipt_expired(now, panel->records[i].collect_until))
            panel->records[kept++] = panel->records[i];
    panel->count = kept;
}

static bool refresh_panel(struct mcp_runtime *runtime, uint64_t now)
{
    struct mcp_panel *panel = &runtime->panel;
    for (unsigned i = 0; i < panel->count; ++i) {
        struct mcp_panel_request *record = &panel->records[i];
        if (record->collect_until)
            continue;
        record->status = runtime->core->button_poll(MCP_ID, record->token);
        if (record->status == AZ_PENDING || record->status == AZ_BUSY)
            return true;
        record->collect_until = mcp_receipt_deadline(now);
    }
    return false;
}

static int new_press(struct mcp_runtime *runtime, struct mcp_panel_request *request, bool pending)
{
    if (request->generation != mcp_live_grant_generation(runtime, az_mcp_clock()))
        return AZ_CANCELLED;
    if (request->view != (uint32_t)runtime->core->view())
        return AZ_STALE;
    if (runtime->panel.count == MCP_PANEL_LIMIT)
        return AZ_LIMIT;
    /* The core only retains the latest token. Collect it before submitting another. */
    if (pending)
        return AZ_BUSY;
    if (request->generation != mcp_live_grant_generation(runtime, az_mcp_clock()))
        return AZ_CANCELLED;
    int status = runtime->core->button_press(MCP_ID, request->button, request->hold, &request->token);
    if (status == AZ_OK) {
        request->status = AZ_PENDING;
        runtime->panel.records[runtime->panel.count++] = *request;
    }
    return status;
}

static int panel_poll(const struct mcp_panel_request *prior, uint64_t *token)
{
    if (!prior)
        return AZ_NOT_FOUND;
    *token = prior->token;
    return prior->status;
}

static int panel_press(struct mcp_runtime *runtime, const cmcp_json_t *args, const char *key,
                       const struct mcp_panel_request *prior, uint64_t *token, bool pending)
{
    struct mcp_panel_request request = {0};
    if (parse_panel(args, &request) != AZ_OK || (prior && !same_panel(prior, &request)))
        return AZ_INVALID;
    if (prior)
        return panel_poll(prior, token);
    snprintf(request.key, sizeof request.key, "%s", key);
    int status = new_press(runtime, &request, pending);
    *token = request.token;
    return status == AZ_OK ? AZ_PENDING : status;
}
static int panel_request(struct mcp_runtime *runtime, const cmcp_json_t *args, uint64_t *token)
{
    const char *key = az_mcp_string(args, "key");
    const char *action = az_mcp_string(args, "action");
    if (!mcp_session_arg(runtime, args) || !action || !key || !key[0] || strlen(key) >= 65)
        return AZ_INVALID;
    uint64_t now = az_mcp_clock();
    mcp_panel_reap(&runtime->panel, now);
    bool pending = refresh_panel(runtime, now);
    const struct mcp_panel_request *prior = panel_find(&runtime->panel, key);
    return !strcmp(action, "status") ? panel_poll(prior, token)
                                     : panel_press(runtime, args, key, prior, token, pending);
}

static int panel_tool(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                      cmcp_json_t **content, int *is_error)
{
    (void)is_error;
    struct mcp_runtime *runtime = user;
    uint64_t token = 0;
    pthread_mutex_lock(&runtime->lock);
    int status = panel_request(runtime, args, &token);
    pthread_mutex_unlock(&runtime->lock);
    cmcp_json_t *out = mcp_sdk_result(runtime, status);
    if (token)
        mcp_set(out, "deliveryToken", mcp_id(token));
    return mcp_tool_result(ctx, out, content);
}

int mcp_register_panel_tools(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    const az_core_v1 *core = runtime->core;
    if (!AZ_HAS(core, button_press) || !AZ_HAS(core, button_poll) || !AZ_HAS(core, view))
        return CMCP_OK;
    const cmcp_tool_t tool = { .name = "deck_input",
        .description = "Deliver a named navigation or deck-layer panel press and poll its delivery.",
        .input_schema = MCP_SCHEMA("\"action\":{\"enum\":[\"press\",\"status\"]},"
            "\"button\":{\"enum\":[\"source\",\"browse\",\"back\",\"menu\",\"search\",\"deck1\",\"deck2\",\"deck3\",\"deck4\"]},"
            "\"expectedView\":{\"enum\":[\"other\",\"source\",\"browse\",\"search\",\"deck\",\"menu\"]},"
            "\"holdMs\":{\"type\":\"integer\",\"minimum\":40,\"maximum\":3000},"
            "\"armGeneration\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4294967295},"
            "\"key\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}," MCP_ARG_SESSION,
            "\"action\",\"sessionId\",\"key\""),
        .handler = panel_tool, .userdata = runtime };
    return cmcp_server_add_tool(server, &tool);
}
