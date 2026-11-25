/* SPDX-License-Identifier: GPL-3.0-only */
#define BEATPORT_MCP
#define main page_regressions
#include "test_beatport_remember.c"
#undef main

static size_t provider_count;
static void (*answer)(int, void *);
static void *answer_user;

int az_mcp_register_tools(const az_core_v1 *owner, const char *id,
                          const az_mcp_tool_v1 *tools, size_t count, az_mcp_provider **out)
{
    assert(owner && !strcmp(id, MOD_ID));
    assert(tools[0].grant == AZ_MCP_READ && tools[1].grant == AZ_MCP_SAVED);
    provider_count = count;
    *out = NULL;
    return AZ_OK;
}

void az_mcp_provider_tick(az_mcp_provider *provider)
{
    (void)provider;
}

static int confirm_forget(const char *id, const char *title, const char *message, const char *yes,
                         void (*callback)(int, void *), void *user)
{
    assert(!strcmp(id, MOD_ID) && title[0] && message[0] && yes[0]);
    answer = callback;
    answer_user = user;
    return 0;
}

int main(void)
{
    assert(page_regressions() == 0 && provider_count == 3);
    static const az_core_v1 tools_core = { .size = sizeof tools_core, .cfg_set = set_cfg,
        .request_redraw = request_redraw, .confirm = confirm_forget };
    C = &tools_core;
    cmcp_json_t *result = NULL;
    strcpy(published.username, "fixture-login");
    assert(mcp_status(NULL, 1, 100, &result, NULL) == AZ_OK);
    assert(!cmcp_json_object_get(result, "username"));
    cmcp_json_free(result);
    cmcp_json_t *change = cmcp_json_parse_cstr("{\"delaySeconds\":50,\"attempts\":2}");
    assert(mcp_options(change, 1, 100, &result, NULL) == AZ_OK && options.max_attempts == 2);
    cmcp_json_free(change);
    cmcp_json_free(result);
    int previous = forget_calls;
    assert(mcp_forget(NULL, 1, 100, &result, NULL) == AZ_OK && answer);
    assert(forget_calls == previous);
    answer(0, answer_user);
    assert(forget_calls == previous);
    answer(1, answer_user);
    assert(forget_calls == previous + 1);
    cmcp_json_free(result);
    puts("Beatport Remember MCP tool tests passed");
    return 0;
}
