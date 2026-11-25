/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"
#include "data-tools.h"
#include "provider/wire.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum payload { NONE, HOT_SLOT, HOT_EDIT, MEMORY_STORE, SELECT_MEMORY, SELECT_ANY, CREATE };
struct cue_action {
    const char *name;
    uint32_t operation, flags;
    enum payload payload;
};

#define SAVED AZ_COMMAND_PERSIST
#define SELECT AZ_COMMAND_NATIVE_CUE_IDS
static const struct cue_action actions[] = {
    { "setMain", AZ_CMD_MAIN_SET, 0, NONE },
    { "recallMain", AZ_CMD_MAIN_RECALL, 0, NONE },
    { "triggerHot", AZ_CMD_HOT_TRIGGER, 0, HOT_SLOT },
    { "captureHot", AZ_CMD_HOT_SET, SAVED, HOT_EDIT },
    { "clearHot", AZ_CMD_HOT_CLEAR, SAVED | SELECT, HOT_EDIT },
    { "storeMemory", AZ_CMD_MEMORY_SET, SAVED, MEMORY_STORE },
    { "recallMemory", AZ_CMD_MEMORY_RECALL, SELECT, SELECT_MEMORY },
    { "deleteMemory", AZ_CMD_MEMORY_DELETE, SAVED | SELECT, SELECT_MEMORY },
    { "recallSavedLoop", AZ_CMD_SAVED_RECALL, SELECT, SELECT_MEMORY },
    { "storeSavedLoop", AZ_CMD_SAVED_SAVE, SAVED, MEMORY_STORE },
    { "deleteSavedLoop", AZ_CMD_SAVED_DELETE, SAVED | SELECT, SELECT_MEMORY },
    { "setSavedLoopAuto", AZ_CMD_SAVED_AUTO, SAVED | SELECT, SELECT_MEMORY },
    { "setComment", AZ_CMD_CUE_COMMENT, SAVED | SELECT, SELECT_ANY },
    { "setColor", AZ_CMD_CUE_COLOR, SAVED | SELECT, SELECT_ANY },
    { "createPoint", AZ_CMD_CUE_POINT_SET, SAVED | SELECT, CREATE },
    { "createLoop", AZ_CMD_CUE_LOOP_SET, SAVED | SELECT, CREATE }
};

static const struct cue_action *find_action(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof actions / sizeof actions[0]; ++i)
        if (!strcmp(actions[i].name, name))
            return &actions[i];
    return NULL;
}

static uint64_t read_revision(const cmcp_json_t *args)
{
    const char *revision = az_mcp_string(args, "cueRevision");
    if (!revision || strspn(revision, "0123456789") != strlen(revision))
        return 0;
    errno = 0;
    uint64_t value = strtoull(revision, NULL, 10);
    return errno ? 0 : value;
}

static uint32_t classification(const cmcp_json_t *args)
{
    const char *kind = az_mcp_string(args, "classification");
    if (kind && !strcmp(kind, "hot"))
        return AZ_CUE_HOT;
    if (kind && !strcmp(kind, "memory"))
        return AZ_CUE_MEMORY;
    return 0;
}

static bool require_id(const cmcp_json_t *args, const char *key, uint32_t limit,
                       struct mcp_intent *intent)
{
    uint64_t id = az_mcp_number(args, key);
    if (!id || id > limit)
        return false;
    intent->slot = (uint32_t)id;
    return true;
}

static bool read_number(const cmcp_json_t *args, const char *key, double *out)
{
    const cmcp_json_t *value = cmcp_json_object_get(args, key);
    if (!value || (value->type != CMCP_JSON_DOUBLE && value->type != CMCP_JSON_INT))
        return false;
    double number = cmcp_json_double(value);
    if (!isfinite(number) || (signbit(number) && fpclassify(number) != FP_ZERO))
        return false;
    *out = fpclassify(number) == FP_ZERO ? 0 : number;
    return true;
}

static bool positions(const cmcp_json_t *args, struct mcp_intent *intent)
{
    if (!read_number(args, "startSeconds", &intent->position))
        return false;
    return intent->operation != AZ_CMD_CUE_LOOP_SET || read_number(args, "endSeconds", &intent->end);
}

static bool select_classified(const cmcp_json_t *args, struct mcp_intent *intent)
{
    intent->classification = classification(args);
    return intent->classification && require_id(args, "entryId",
        intent->classification == AZ_CUE_HOT ? 8 : AZ_NATIVE_CUE_LIMIT, intent);
}

static bool select_creation(const cmcp_json_t *args, struct mcp_intent *intent)
{
    intent->classification = classification(args);
    if (!intent->classification || !positions(args, intent))
        return false;
    return intent->classification == AZ_CUE_MEMORY || require_id(args, "slot", 8, intent);
}

static bool selector(const cmcp_json_t *args, struct mcp_intent *intent, enum payload payload)
{
    switch (payload) {
    case NONE:
        return !cmcp_json_object_get(args, "slot");
    case HOT_EDIT:
        intent->classification = AZ_CUE_HOT;
        return require_id(args, "slot", 8, intent);
    case HOT_SLOT:
        return require_id(args, "slot", 8, intent);
    case MEMORY_STORE:
        intent->classification = AZ_CUE_MEMORY;
        return true;
    case SELECT_MEMORY:
        intent->classification = AZ_CUE_MEMORY;
        return require_id(args, "entryId", AZ_NATIVE_CUE_LIMIT, intent);
    case SELECT_ANY:
        return select_classified(args, intent);
    case CREATE:
        return select_creation(args, intent);
    }
    return false;
}

static bool read_rgb(const cmcp_json_t *args, struct mcp_intent *intent)
{
    const cmcp_json_t *color = cmcp_json_object_get(args, "rgb");
    if (!color)
        return intent->operation != AZ_CMD_CUE_COLOR;
    if (color->type != CMCP_JSON_INT || color->i < 0 || color->i > 0xffffff)
        return false;
    intent->rgb = (uint32_t)color->i;
    return true;
}

static bool read_comment(const cmcp_json_t *args, struct mcp_intent *intent)
{
    const char *comment = az_mcp_string(args, "comment");
    if (!comment)
        return intent->operation != AZ_CMD_CUE_COMMENT;
    if (strlen(comment) >= sizeof intent->comment)
        return false;
    memcpy(intent->comment, comment, strlen(comment) + 1);
    return true;
}

static bool read_auto(const cmcp_json_t *args, struct mcp_intent *intent)
{
    const cmcp_json_t *enabled = cmcp_json_object_get(args, "autoActive");
    if (!enabled || enabled->type != CMCP_JSON_BOOL)
        return false;
    intent->position = cmcp_json_bool(enabled) ? 1 : 0;
    return true;
}

static bool metadata(const cmcp_json_t *args, struct mcp_intent *intent, enum payload payload)
{
    if (payload == CREATE)
        return read_comment(args, intent) && read_rgb(args, intent);
    switch (intent->operation) {
    case AZ_CMD_CUE_COMMENT: return read_comment(args, intent);
    case AZ_CMD_HOT_SET:
    case AZ_CMD_CUE_COLOR: return read_rgb(args, intent);
    case AZ_CMD_SAVED_AUTO: return read_auto(args, intent);
    default: return true;
    }
}

int mcp_cue_intent(const cmcp_json_t *args, struct mcp_intent *intent)
{
    const struct cue_action *action = find_action(az_mcp_string(args, "action"));
    if (!action)
        return AZ_INVALID;
    intent->operation = action->operation;
    intent->flags = action->flags;
    if (action->flags) {
        intent->cue_revision = read_revision(args);
        if (!intent->cue_revision)
            return AZ_INVALID;
    }
    return selector(args, intent, action->payload) && metadata(args, intent, action->payload)
        ? AZ_OK : AZ_INVALID;
}

static cmcp_json_t *native_cues_json(const az_native_cue_snapshot *bank)
{
    cmcp_json_t *out = cmcp_json_new_object();
    cmcp_json_t *entries = cmcp_json_new_array();
    mcp_set(out, "loadEpoch", mcp_id(bank->native_load_generation));
    mcp_set(out, "cueRevision", mcp_id(bank->revision));
    mcp_set(out, "sourceRateHz", cmcp_json_new_int(bank->source_rate_hz));
    for (uint32_t i = 0; i < bank->count; ++i) {
        cmcp_json_t *entry = mcp_cue_entry_json(&bank->entries[i]);
        if (cmcp_json_array_append(entries, entry))
            cmcp_json_free(entry);
    }
    mcp_set(out, "entries", entries);
    return out;
}

static int native_cues(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                        cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    unsigned deck = mcp_input_deck(args);
    if (!deck)
        return mcp_tool_error("A logical deck from 1 to 4 must be selected.", content, is_error);
    az_native_cue_snapshot native = { .size = sizeof native };
    int status = runtime->observation->native_cues((int)deck - 1, &native);
    if (status == AZ_OK && native.count > AZ_NATIVE_CUE_LIMIT)
        status = AZ_FAILED;
    cmcp_json_t *out = mcp_session_result(runtime);
    mcp_set(out, "status", cmcp_json_new_int(status));
    if (status == AZ_OK)
        mcp_set(out, "bank", native_cues_json(&native));
    return mcp_tool_result(ctx, out, content);
}

int mcp_register_native_cues(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    if (!AZ_OBSERVATION_HAS(runtime->observation, native_cues))
        return CMCP_OK;
    const cmcp_tool_t tool = { .name = "deck_native_cues",
        .description = "Read the native cue IDs and exact revision used for editing.",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"deck\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4}},"
                        "\"required\":[\"deck\"],\"additionalProperties\":false}",
        .handler = native_cues, .userdata = runtime };
    return cmcp_server_add_tool(server, &tool);
}

static bool batch_entry(const cmcp_json_t *input, az_cue_batch_entry *out)
{
    struct mcp_intent entry = {0};
    const char *type = az_mcp_string(input, "type");
    if (!type || (strcmp(type, "point") && strcmp(type, "loop")))
        return false;
    entry.operation = !strcmp(type, "loop") ? AZ_CMD_CUE_LOOP_SET : AZ_CMD_CUE_POINT_SET;
    if (!selector(input, &entry, CREATE) || !metadata(input, &entry, CREATE))
        return false;
    out->type = entry.operation == AZ_CMD_CUE_LOOP_SET ? AZ_CUE_LOOP : AZ_CUE_POINT;
    out->classification = entry.classification;
    out->slot = entry.slot;
    out->rgb = entry.rgb;
    out->position_s = entry.position;
    out->end_s = entry.end;
    memcpy(out->comment, entry.comment, sizeof out->comment);
    return true;
}

static bool batch_args(const cmcp_json_t *args, struct mcp_intent *intent)
{
    const char *mode = az_mcp_string(args, "mode");
    const cmcp_json_t *entries = cmcp_json_object_get(args, "entries");
    size_t count = cmcp_json_array_len(entries);
    if (!mode || !count || count > AZ_CUE_BATCH_LIMIT)
        return false;
    intent->batch_mode = !strcmp(mode, "replace") ? AZ_CUE_BATCH_REPLACE : AZ_CUE_BATCH_IF_EMPTY;
    intent->batch_count = (uint32_t)count;
    intent->flags = AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_CUE_IDS;
    intent->cue_revision = read_revision(args);
    if (!intent->cue_revision)
        return false;
    for (size_t i = 0; i < count; ++i)
        if (!batch_entry(cmcp_json_array_at(entries, i), &intent->batch[i]))
            return false;
    return true;
}

static int apply_bank(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                       cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    struct mcp_intent intent;
    if (!mcp_session_arg(runtime, args) || !mcp_base_intent(args, &intent) || !batch_args(args, &intent))
        return mcp_tool_error("Invalid cue bank, session, or target revision.", content, is_error);
    return mcp_submit_tool(args, runtime, &intent, ctx, content, is_error);
}

int mcp_register_cue_batch(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    if (!AZ_COMMANDS_HAS(runtime->commands.api, cue_batch_submit_async))
        return CMCP_OK;
    const cmcp_tool_t tool = { .name = "deck_apply_cue_bank",
        .description = "Apply a complete cue bank. Replacement asks for confirmation on the deck.",
        .input_schema = MCP_SCHEMA(MCP_ARG_DECK "," MCP_ARG_EPOCH "," MCP_ARG_SESSION ","
            "\"key\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64},"
            "\"armGeneration\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4294967295},"
            "\"cueRevision\":{\"type\":\"string\",\"maxLength\":20},"
            "\"mode\":{\"enum\":[\"ifEmpty\",\"replace\"]},"
            "\"entries\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":18,\"items\":{\"type\":\"object\"}}",
            "\"deck\",\"loadEpoch\",\"sessionId\",\"key\",\"armGeneration\",\"cueRevision\",\"mode\",\"entries\""),
        .handler = apply_bank, .userdata = runtime };
    return cmcp_server_add_tool(server, &tool);
}
