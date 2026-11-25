/* SPDX-License-Identifier: GPL-3.0-only
 * Included by anchor.c so the provider uses the same UI-owned guide state. */
#include "wire.h"
#include <errno.h>
#include <stdlib.h>

static az_mcp_provider *anchor_provider;

static void anchor_int(cmcp_json_t *object, const char *key, long long value)
{
    az_mcp_set(object, key, cmcp_json_new_int(value));
}

static void anchor_text(cmcp_json_t *object, const char *key, const char *value)
{
    az_mcp_set(object, key, cmcp_json_new_string(value));
}

static int mcp_read_guide(const cmcp_json_t *args, uint64_t call, uint64_t deadline,
                          cmcp_json_t **out, void *user)
{
    (void)args;
    (void)call;
    (void)deadline;
    (void)user;
    *out = cmcp_json_new_object();
    anchor_int(*out, "outgoingDeck", cur_a + 1);
    anchor_int(*out, "incomingDeck", cur_b + 1);
    anchor_int(*out, "state", plan.state);
    anchor_int(*out, "notice", plan.notice);
    anchor_int(*out, "loops", plan.loops);
    anchor_int(*out, "windowOpen", plan.window_open);
    anchor_int(*out, "outgoingTargetMs", plan.target_ms);
    anchor_int(*out, "incomingTargetMs", plan.b_target_ms);
    anchor_text(*out, "outgoingTarget", plan.target);
    anchor_text(*out, "incomingTarget", plan.b_target);
    anchor_text(*out, "instruction", plan.line1);
    anchor_text(*out, "detail", plan.line2);
    return AZ_OK;
}

static uint64_t anchor_load(const cmcp_json_t *args)
{
    const char *text = az_mcp_string(args, "loadEpoch");
    if (!text || strspn(text, "0123456789") != strlen(text))
        return 0;
    errno = 0;
    uint64_t value = strtoull(text, NULL, 10);
    return errno ? 0 : value;
}

static int anchor_deck(const cmcp_json_t *args, bool *incoming)
{
    const char *role = az_mcp_string(args, "role");
    if (!role)
        return -1;
    *incoming = !strcmp(role, "incoming");
    return *incoming ? cur_b : cur_a;
}

static int target_kind(const cmcp_json_t *args)
{
    const char *kind = az_mcp_string(args, "kind");
    return kind && !strcmp(kind, "hotCues");
}

static int anchor_candidates(int deck, bool incoming, const cmcp_json_t *args, an_cand_t *out)
{
    const an_deck_status_t *status = &decks[deck];
    if (incoming)
        return an_b_candidates(&status->analysis, target_kind(args), out, MAX_CAND);
    return status->position_valid
        ? an_candidates(&status->analysis, status->pos_ms, status->sample_rate, &cur_opts, out, MAX_CAND) : 0;
}

static int mcp_list_targets(const cmcp_json_t *args, uint64_t call, uint64_t deadline,
                            cmcp_json_t **out, void *user)
{
    (void)call;
    (void)deadline;
    (void)user;
    bool incoming;
    int deck = anchor_deck(args, &incoming);
    if (deck < 0 || !decks[deck].loaded)
        return AZ_BUSY;
    an_cand_t candidates[MAX_CAND];
    int count = anchor_candidates(deck, incoming, args, candidates);
    cmcp_json_t *rows = cmcp_json_new_array();
    for (int i = 0; i < count; ++i) {
        cmcp_json_t *entry = cmcp_json_new_object();
        anchor_int(entry, "positionMs", candidates[i].ms);
        anchor_text(entry, "label", candidates[i].label);
        if (cmcp_json_array_append(rows, entry))
            cmcp_json_free(entry);
    }
    char epoch[24];
    snprintf(epoch, sizeof epoch, "%llu", (unsigned long long)decks[deck].epoch);
    *out = cmcp_json_new_object();
    anchor_int(*out, "deck", deck + 1);
    anchor_text(*out, "loadEpoch", epoch);
    az_mcp_set(*out, "targets", rows);
    return AZ_OK;
}

static void anchor_set_candidate(bool incoming, const an_cand_t *candidate)
{
    if (incoming) {
        apply_b_target(candidate);
        return;
    }
    latched_ms = candidate->ms;
    snprintf(latched_label, sizeof latched_label, "%s", candidate->label);
    if (cur_b >= 0)
        decks[cur_b].loop.released = 0;
    close_selector();
    shown_display_valid = 0;
    update_guide(mono_ms());
}

static const an_cand_t *find_candidate(const an_cand_t *list, int count, uint32_t position, uint32_t rate)
{
    for (int i = 0; i < count; ++i) {
        int order = compare_timestamps(list[i].ms, position, 1000, rate, 1);
        if (order == AZ_TIME_INVALID)
            return NULL;
        if (order == AZ_TIME_EQUAL)
            return &list[i];
    }
    return NULL;
}

static int mcp_set_target(const cmcp_json_t *args, uint64_t call, uint64_t deadline,
                          cmcp_json_t **out, void *user)
{
    bool incoming;
    int deck = anchor_deck(args, &incoming);
    uint64_t load = anchor_load(args);
    long long position = cmcp_json_int(cmcp_json_object_get(args, "positionMs"));
    if (deck < 0 || !load || decks[deck].epoch != load || position < 0 || position > UINT32_MAX)
        return AZ_STALE;
    an_cand_t candidates[MAX_CAND];
    int count = anchor_candidates(deck, incoming, args, candidates);
    const an_cand_t *selected = find_candidate(candidates, count, (uint32_t)position, decks[deck].sample_rate);
    if (!selected)
        return AZ_STALE;
    anchor_set_candidate(incoming, selected);
    return mcp_read_guide(args, call, deadline, out, user);
}

static int mcp_settings_read(const cmcp_json_t *args, uint64_t call, uint64_t deadline,
                             cmcp_json_t **out, void *user)
{
    (void)args;
    (void)call;
    (void)deadline;
    (void)user;
    *out = cmcp_json_new_object();
    for (int i = 0; i < SET_COUNT; ++i)
        anchor_int(*out, settings_key(i), settings_get(i));
    return AZ_OK;
}

static int mcp_settings_write(const cmcp_json_t *args, uint64_t call, uint64_t deadline,
                              cmcp_json_t **out, void *user)
{
    const char *key = az_mcp_string(args, "setting");
    long long value = cmcp_json_int(cmcp_json_object_get(args, "value"));
    long long expected = cmcp_json_int(cmcp_json_object_get(args, "expected"));
    for (int i = 0; key && i < SET_COUNT; ++i) {
        if (strcmp(key, settings_key(i)))
            continue;
        if (settings_get(i) != expected)
            return AZ_STALE;
        if (value < 0 || value >= settings_choices(i))
            return AZ_INVALID;
        settings_set(i, (int)value);
        return mcp_settings_read(args, call, deadline, out, user);
    }
    return AZ_NOT_FOUND;
}

#define AN_ROLE "\"role\":{\"enum\":[\"incoming\",\"outgoing\"]},\"kind\":{\"enum\":[\"phrases\",\"hotCues\"]}"
#define AN_KEY "\"key\":{\"type\":\"string\",\"maxLength\":64}"
#define AN_ARGS(properties, required) "{\"type\":\"object\",\"properties\":{" properties "},\"required\":[" required "],\"additionalProperties\":false}"
#define AN_TOOL(id, scope, summary, function, props, req) \
    { sizeof(az_mcp_tool_v1), id, NULL, summary, AN_ARGS(props, req), "{\"type\":\"object\"}", scope, function, NULL }

static void anchor_mcp_register(void)
{
    static const az_mcp_tool_v1 tools[] = {
        AN_TOOL("read_guide", AZ_MCP_READ, "Read the active transition guide.", mcp_read_guide, "", ""),
        AN_TOOL("list_targets", AZ_MCP_READ, "List qualified targets for the active deck role.", mcp_list_targets,
          AN_ROLE, "\"role\""),
        AN_TOOL("set_target", AZ_MCP_LIVE, "Select a listed target on the same deck load.", mcp_set_target,
          AN_ROLE "," AN_KEY ",\"loadEpoch\":{\"type\":\"string\",\"maxLength\":20},"
          "\"positionMs\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":4294967295}",
          "\"role\",\"key\",\"loadEpoch\",\"positionMs\""),
        AN_TOOL("get_settings", AZ_MCP_READ, "Read Anchor settings.", mcp_settings_read, "", ""),
        AN_TOOL("set_settings", AZ_MCP_SAVED, "Change one Anchor setting.", mcp_settings_write,
          AN_KEY ",\"setting\":{\"type\":\"string\"},\"value\":{\"type\":\"integer\"},\"expected\":{\"type\":\"integer\"}",
          "\"key\",\"setting\",\"value\",\"expected\"")
    };
    if (az_mcp_register_tools(C, MOD_ID, tools, sizeof tools / sizeof tools[0], &anchor_provider) != AZ_OK &&
        AZ_HAS(C, log))
        C->log(MOD_ID, "MCP provider startup failed");
}

static void anchor_mcp_tick(void)
{
    az_mcp_provider_tick(anchor_provider);
}
