/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"
#include "data-tools.h"
#include "provider/wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DECK_ARG "\"deck\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4}"
#define INPUT(fields, required) "{\"type\":\"object\",\"additionalProperties\":false," \
    "\"properties\":{" fields "},\"required\":[" required "]}"
#define OBJECT_RESULT "{\"type\":\"object\"}"

int mcp_tool_result(cmcp_handler_ctx_t *ctx, cmcp_json_t *data, cmcp_json_t **content)
{
    if (!data)
        return CMCP_ENOMEM;
    char *text = cmcp_json_emit(data);
    *content = text ? cmcp_tool_text_content(text) : NULL;
    free(text);
    cmcp_handler_set_structured(ctx, data);
    return *content ? CMCP_OK : CMCP_ENOMEM;
}

int mcp_tool_error(const char *message, cmcp_json_t **content, int *is_error)
{
    *is_error = 1;
    *content = cmcp_tool_text_content(message);
    return *content ? CMCP_OK : CMCP_ENOMEM;
}

cmcp_json_t *mcp_session_result(const struct mcp_runtime *runtime)
{
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "sessionId", cmcp_json_new_string(runtime->session));
    return out;
}

static int capabilities(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                        cmcp_json_t **content, int *is_error)
{
    (void)args;
    (void)is_error;
    struct mcp_runtime *runtime = user;
    cmcp_json_t *out = mcp_session_result(runtime);
    mcp_set(out, "firmware", cmcp_json_new_string(runtime->firmware));
    mcp_set(out, "deckCount", cmcp_json_new_int(MCP_DECKS));
    mcp_set(out, "positionHz", cmcp_json_new_int(5));
    mcp_set(out, "statusTopics", mcp_id(mcp_watch_topics()));
    mcp_set(out, "commandAccess", cmcp_json_new_bool(runtime->commands.api != NULL));
    mcp_set(out, "armGeneration", cmcp_json_new_int(atomic_load(&runtime->commands.grant)));
    unsigned saved = az_mcp_before(az_mcp_clock(), atomic_load(&runtime->saved_edits))
        ? atomic_load(&runtime->saved_generation) : 0;
    mcp_set(out, "savedArmGeneration", cmcp_json_new_int(saved));
    return mcp_tool_result(ctx, out, content);
}

static void copy_deck(struct mcp_runtime *runtime, unsigned deck, struct mcp_deck *snapshot)
{
    pthread_mutex_lock(&runtime->lock);
    *snapshot = runtime->published[deck - 1];
    pthread_mutex_unlock(&runtime->lock);
}

unsigned mcp_input_deck(const cmcp_json_t *args)
{
    long long deck = cmcp_json_int(cmcp_json_object_get(args, "deck"));
    return deck >= 1 && deck <= MCP_DECKS ? (unsigned)deck : 0;
}

static int read_state(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                      cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    unsigned deck = mcp_input_deck(args);
    if (!deck)
        return mcp_tool_error("Deck must be in the range 1 to 4.", content, is_error);
    struct mcp_deck *snapshot = malloc(sizeof *snapshot);
    if (!snapshot)
        return mcp_tool_error("Snapshot allocation failed.", content, is_error);
    copy_deck(runtime, deck, snapshot);
    cmcp_json_t *out = mcp_session_result(runtime);
    mcp_set(out, "snapshot", mcp_deck_json(snapshot, (int)deck));
    free(snapshot);
    return mcp_tool_result(ctx, out, content);
}

static int cue_bank(const cmcp_json_t *args)
{
    const cmcp_json_t *value = cmcp_json_object_get(args, "bank");
    const char *bank = cmcp_json_string(value);
    for (int i = 0; i < MCP_CUE_BANKS; ++i) {
        size_t len = strlen(mcp_cue_names[i]);
        if (bank && cmcp_json_string_len(value) == len && !memcmp(bank, mcp_cue_names[i], len))
            return i;
    }
    return -1;
}

static bool matches_id(const cmcp_json_t *arg, uint64_t id)
{
    const char *value = cmcp_json_string(arg);
    char expected[24];
    int length = snprintf(expected, sizeof expected, "%llu", (unsigned long long)id);
    return value && cmcp_json_string_len(arg) == (size_t)length && !memcmp(value, expected, (size_t)length);
}

static bool same_bank(const cmcp_json_t *args, const struct mcp_deck *snapshot, int bank)
{
    return matches_id(cmcp_json_object_get(args, "loadEpoch"), snapshot->load_epoch) &&
           matches_id(cmcp_json_object_get(args, "revision"), snapshot->cues[bank].receipt.info.revision) &&
           matches_id(cmcp_json_object_get(args, "providerId"), snapshot->cues[bank].receipt.provider_id);
}

static int cue_page(const cmcp_json_t *args, struct mcp_runtime *runtime,
                    const struct mcp_deck *snapshot, int bank, unsigned deck,
                    cmcp_handler_ctx_t *ctx, cmcp_json_t **content, int *is_error)
{
    size_t offset = (size_t)cmcp_json_int(cmcp_json_object_get(args, "offset"));
    if (offset && !same_bank(args, snapshot, bank))
        return mcp_tool_error("Cue page expired. Read again from offset zero.", content, is_error);
    const cmcp_json_t *requested_limit = cmcp_json_object_get(args, "limit");
    size_t limit = requested_limit ? (size_t)cmcp_json_int(requested_limit) : 16;
    cmcp_json_t *out = mcp_session_result(runtime);
    mcp_set(out, "deck", cmcp_json_new_int(deck));
    mcp_set(out, "page", mcp_cues_json(snapshot, bank, offset, limit));
    return mcp_tool_result(ctx, out, content);
}

static int read_cues(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                     cmcp_json_t **content, int *is_error)
{
    int bank = cue_bank(args);
    if (bank < 0)
        return mcp_tool_error("Unknown cue bank.", content, is_error);
    struct mcp_runtime *runtime = user;
    unsigned deck = mcp_input_deck(args);
    if (!deck)
        return mcp_tool_error("Deck must be in the range 1 to 4.", content, is_error);
    struct mcp_deck *snapshot = malloc(sizeof *snapshot);
    if (!snapshot)
        return mcp_tool_error("Snapshot allocation failed.", content, is_error);
    copy_deck(runtime, deck, snapshot);
    int status = cue_page(args, runtime, snapshot, bank, deck, ctx, content, is_error);
    free(snapshot);
    return status;
}

int mcp_register_tools(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    const cmcp_tool_t tools[] = {
        {
            .name = "deck_capabilities",
            .description = "Read the deck session, firmware and supported observation surface.",
            .input_schema = INPUT("", ""),
            .output_schema = OBJECT_RESULT,
            .handler = capabilities,
            .userdata = runtime
        },
        {
            .name = "deck_read_state",
            .description = "Observe a logical deck (1–4). Field receipts report native availability and clock.",
            .input_schema = INPUT(DECK_ARG, "\"deck\""),
            .output_schema = OBJECT_RESULT,
            .handler = read_state,
            .userdata = runtime
        },
        {
            .name = "deck_read_cues",
            .description = "Read a cue bank. For the next page use nextOffset, loadEpoch, revision and providerId from the previous page.",
            .input_schema = INPUT(DECK_ARG ","
                "\"bank\":{\"enum\":[\"main\",\"liveHot\",\"liveMemory\",\"savedHot\",\"savedMemory\"]},"
                "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":64},"
                "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32},"
                "\"loadEpoch\":{\"type\":\"string\",\"maxLength\":20},"
                "\"revision\":{\"type\":\"string\",\"maxLength\":20},"
                "\"providerId\":{\"type\":\"string\",\"maxLength\":20}", "\"deck\",\"bank\""),
            .output_schema = OBJECT_RESULT,
            .handler = read_cues,
            .userdata = runtime
        }
    };
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; ++i) {
        int status = cmcp_server_add_tool(server, &tools[i]);
        if (status != CMCP_OK)
            return status;
    }
    int (*const register_optional[])(cmcp_server_t *, struct mcp_runtime *) = {
        mcp_register_native_cues, mcp_register_library_tools, mcp_register_stored_tools,
        mcp_register_analysis_tools, mcp_register_panel_tools, mcp_register_settings_tools,
        mcp_register_live_tools
    };
    for (size_t i = 0; i < sizeof register_optional / sizeof register_optional[0]; ++i) {
        int status = register_optional[i](server, runtime);
        if (status != CMCP_OK)
            return status;
    }
    return runtime->commands.api ? mcp_register_command_tools(server, runtime) : CMCP_OK;
}
