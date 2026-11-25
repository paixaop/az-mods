/* SPDX-License-Identifier: GPL-3.0-only */
#define ANCHOR_MCP
#define main deck_regressions
#include "test_lifecycle.c"
#undef main

static const az_mcp_tool_v1 *registered_tools;
static size_t registered_count;

int az_mcp_register_tools(const az_core_v1 *owner, const char *id,
                          const az_mcp_tool_v1 *tools, size_t count, az_mcp_provider **out)
{
    assert(owner && !strcmp(id, MOD_ID));
    registered_tools = tools;
    registered_count = count;
    *out = NULL;
    return AZ_OK;
}

void az_mcp_provider_tick(az_mcp_provider *provider)
{
    (void)provider;
}

static cmcp_json_t *call(const char *name, const char *args, int expected)
{
    cmcp_json_t *json = cmcp_json_parse_cstr(args);
    cmcp_json_t *out = NULL;
    assert(json);
    for (size_t i = 0; i < registered_count; ++i) {
        const az_mcp_tool_v1 *tool = &registered_tools[i];
        if (!strcmp(tool->name, name)) {
            assert(tool->invoke(json, 1, 900, &out, tool->user) == expected);
            cmcp_json_free(json);
            return out;
        }
    }
    assert(0);
}

int main(void)
{
    assert(deck_regressions() == 0 && registered_count == 5);
    C = &core;
    cur_a = 0;
    cur_b = 1;
    static const an_cue_t cue[] = {{ .time_ms = 16000, .slot = 1, .label = "HOT" }};
    decks[1].loaded = 1;
    decks[1].epoch = 17;
    decks[1].sample_rate = 48000;
    decks[1].analysis.cues = cue;
    decks[1].analysis.ncues = 1;

    cmcp_json_t *result = call("read_guide", "{}", AZ_OK);
    assert(cmcp_json_int(cmcp_json_object_get(result, "incomingDeck")) == 2);
    cmcp_json_free(result);
    result = call("list_targets", "{\"role\":\"incoming\",\"kind\":\"hotCues\"}", AZ_OK);
    assert(cmcp_json_array_len(cmcp_json_object_get(result, "targets")) == 1);
    cmcp_json_free(result);
    cmcp_json_free(call("set_target", "{\"role\":\"incoming\",\"kind\":\"hotCues\","
                                     "\"loadEpoch\":\"16\",\"positionMs\":16000}", AZ_STALE));
    cmcp_json_free(call("set_target", "{\"role\":\"incoming\",\"kind\":\"hotCues\","
                                     "\"loadEpoch\":\"17\",\"positionMs\":16000}", AZ_OK));
    int original = settings_get(SET_SHOW);
    result = call("get_settings", "{}", AZ_OK);
    assert(cmcp_json_int(cmcp_json_object_get(result, "show")) == original);
    cmcp_json_free(result);
    char change[128];
    snprintf(change, sizeof change, "{\"setting\":\"show\",\"expected\":%d,\"value\":%d}", original, !original);
    cmcp_json_free(call("set_settings", change, AZ_OK));
    assert(settings_get(SET_SHOW) == !original);
    puts("Anchor MCP tool tests passed");
    return 0;
}
