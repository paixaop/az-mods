/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"
#include "data-tools.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ACTION_FIELDS \
    "\"sessionId\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64}," \
    "\"key\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}"
#define TARGET_FIELDS ACTION_FIELDS "," \
    "\"deck\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4}," \
    "\"armGeneration\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4294967295}," \
    "\"loadEpoch\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":20},"
#define TARGET_REQUIRED "\"sessionId\",\"key\",\"deck\",\"armGeneration\",\"loadEpoch\",\"action\""
#define SCHEMA(fields, required) "{\"type\":\"object\",\"additionalProperties\":false," \
    "\"properties\":{" fields "},\"required\":[" required "]}"

struct action {
    const char *name;
    uint32_t id;
};
static const struct action loop_actions[] = {
    { "in", AZ_CMD_LOOP_IN }, { "out", AZ_CMD_LOOP_OUT }, { "enter", AZ_CMD_LOOP_ENTER },
    { "exit", AZ_CMD_LOOP_EXIT }, { "reloop", AZ_CMD_RELOOP }, { "clear", AZ_CMD_LOOP_CLEAR },
    { "double", AZ_CMD_LOOP_DOUBLE }, { "halve", AZ_CMD_LOOP_HALVE }, { "length", AZ_CMD_LOOP_LENGTH },
    { "move", AZ_CMD_LOOP_MOVE }, { "adjustIn", AZ_CMD_LOOP_ADJUST_IN }, { "adjustOut", AZ_CMD_LOOP_ADJUST_OUT },
    { NULL, 0 }
};
static const char *string_arg(const cmcp_json_t *args, const char *key)
{
    const cmcp_json_t *value = cmcp_json_object_get(args, key);
    const char *str = cmcp_json_string(value);
    return str && strlen(str) == cmcp_json_string_len(value) ? str : "";
}

static bool valid_key(const char *key)
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-";
    size_t length = strlen(key);
    return length && length < MCP_KEY_SIZE && strspn(key, alphabet) == length;
}

static uint64_t epoch_arg(const cmcp_json_t *args)
{
    const char *str = string_arg(args, "loadEpoch");
    if (!*str || strspn(str, "0123456789") != strlen(str))
        return 0;
    errno = 0;
    uint64_t value = strtoull(str, NULL, 10);
    return errno ? 0 : value;
}

static uint32_t find_action(const struct action *actions, const char *name)
{
    for (; actions->name; ++actions)
        if (!strcmp(actions->name, name))
            return actions->id;
    return 0;
}

static bool matches_session(const struct mcp_runtime *runtime, const cmcp_json_t *args)
{
    return !strcmp(runtime->session, string_arg(args, "sessionId"));
}

static bool parse_amount(const cmcp_json_t *args, uint32_t operation, double *amount)
{
    const cmcp_json_t *arg = cmcp_json_object_get(args, "amount");
    *amount = 0;
    if (operation < AZ_CMD_LOOP_LENGTH || operation > AZ_CMD_LOOP_ADJUST_OUT)
        return !arg;
    if (!arg)
        return false;
    double value = cmcp_json_double(arg);
    if (!isfinite(value) || fpclassify(value) == FP_ZERO)
        return false;
    if (operation == AZ_CMD_LOOP_LENGTH && signbit(value))
        return false;
    *amount = value;
    return true;
}

bool mcp_base_intent(const cmcp_json_t *args, struct mcp_intent *intent)
{
    const char *key = string_arg(args, "key");
    unsigned deck = mcp_input_deck(args);
    uint64_t load = epoch_arg(args);
    if (!deck || !load || !valid_key(key))
        return false;
    *intent = (struct mcp_intent){ .deck = deck - 1, .load = load };
    memcpy(intent->key, key, strlen(key) + 1);
    return true;
}

static cmcp_json_t *job_json(struct mcp_runtime *runtime, const struct mcp_job *job)
{
    static const char *const states[] = { "queued", "accepted", "applied", "rejected", "cancelled", "uncertain" };
    cmcp_json_t *out = mcp_session_result(runtime);
    mcp_set(out, "key", cmcp_json_new_string(job->intent.key));
    mcp_set(out, "state", cmcp_json_new_string(states[job->state]));
    mcp_set(out, "reason", cmcp_json_new_int(job->reason));
    mcp_set(out, "settled", cmcp_json_new_bool(job->settled));
    mcp_set(out, "cancelRequested", cmcp_json_new_bool(job->cancel_requested));
    mcp_set(out, "deck", cmcp_json_new_int(job->intent.deck + 1));
    mcp_set(out, "loadEpoch", mcp_id(job->intent.load));
    mcp_set(out, "revision", mcp_id(job->revision));
    mcp_set(out, "observedNs", mcp_id(job->observed_ns));
    return out;
}

static const char *refusal(int status)
{
    switch (status) {
    case AZ_INVALID: return "Idempotency key already belongs to a different request or grant.";
    case AZ_CANCELLED: return "Live control is disarmed or this grant has expired.";
    case AZ_LIMIT: return "The session command ledger is full.";
    default: return "The command could not be queued.";
    }
}

int mcp_submit_tool(const cmcp_json_t *args, struct mcp_runtime *runtime, const struct mcp_intent *intent,
                    cmcp_handler_ctx_t *ctx, cmcp_json_t **content, int *is_error)
{
    unsigned grant = (unsigned)cmcp_json_int(cmcp_json_object_get(args, "armGeneration"));
    struct mcp_job job;
    int status = mcp_queue(runtime, intent, grant, &job);
    if (status != AZ_OK)
        return mcp_tool_error(refusal(status), content, is_error);
    return mcp_tool_result(ctx, job_json(runtime, &job), content);
}

static int submit(const cmcp_json_t *args, struct mcp_runtime *runtime, const struct action *actions,
                  cmcp_handler_ctx_t *ctx, cmcp_json_t **content, int *is_error)
{
    struct mcp_intent intent;
    if (!matches_session(runtime, args) || !mcp_base_intent(args, &intent))
        return mcp_tool_error("Invalid deck target, action or session.", content, is_error);
    if (actions) {
        intent.operation = find_action(actions, string_arg(args, "action"));
        if (!intent.operation || !parse_amount(args, intent.operation, &intent.amount))
            return mcp_tool_error("Invalid loop action or amount.", content, is_error);
    } else if (mcp_cue_intent(args, &intent) != AZ_OK)
        return mcp_tool_error("Invalid cue action or target revision.", content, is_error);
    return mcp_submit_tool(args, runtime, &intent, ctx, content, is_error);
}

static int loop(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                cmcp_json_t **content, int *is_error)
{
    return submit(args, user, loop_actions, ctx, content, is_error);
}

static int cue(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
               cmcp_json_t **content, int *is_error)
{
    return submit(args, user, NULL, ctx, content, is_error);
}

static int command_status(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                          cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    struct mcp_job job;
    if (!matches_session(runtime, args) || !mcp_job_status(runtime, string_arg(args, "key"), &job))
        return mcp_tool_error("Command not found in this deck session.", content, is_error);
    return mcp_tool_result(ctx, job_json(runtime, &job), content);
}

static int cancel_command(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                          cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    struct mcp_job job;
    if (!matches_session(runtime, args) || !mcp_cancel_job(runtime, string_arg(args, "key"), &job))
        return mcp_tool_error("Command not found in this deck session.", content, is_error);
    return mcp_tool_result(ctx, job_json(runtime, &job), content);
}

int mcp_register_command_tools(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    const cmcp_tool_t tools[] = {
        {
            .name = "deck_loop",
            .description = "Queue a native loop action. Amount is in beats for length and signed source seconds for move, adjustIn and adjustOut. Check the result with deck_command_status.",
            .input_schema = SCHEMA(TARGET_FIELDS "\"action\":{\"enum\":[\"in\",\"out\",\"enter\",\"exit\",\"reloop\",\"clear\",\"double\","
                "\"halve\",\"length\",\"move\",\"adjustIn\",\"adjustOut\"]},"
                "\"amount\":{\"type\":\"number\",\"minimum\":-65535,\"maximum\":65535}", TARGET_REQUIRED),
            .output_schema = "{\"type\":\"object\"}",
            .handler = loop, .userdata = runtime
        },
        {
            .name = "deck_cue",
            .description = "Control live and saved cues. Use the native cue revision for saved targets.",
            .input_schema = SCHEMA(TARGET_FIELDS "\"action\":{\"enum\":[\"setMain\",\"recallMain\",\"triggerHot\","
                "\"captureHot\",\"clearHot\",\"storeMemory\",\"recallMemory\",\"deleteMemory\",\"recallSavedLoop\","
                "\"storeSavedLoop\",\"deleteSavedLoop\",\"setSavedLoopAuto\",\"setComment\",\"setColor\","
                "\"createPoint\",\"createLoop\"]},\"slot\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8},"
                "\"cueRevision\":{\"type\":\"string\",\"maxLength\":20},"
                "\"entryId\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":64},"
                "\"classification\":{\"enum\":[\"hot\",\"memory\"]},"
                "\"autoActive\":{\"type\":\"boolean\"},\"comment\":{\"type\":\"string\",\"maxLength\":127},"
                "\"rgb\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":16777215},"
                "\"startSeconds\":{\"type\":\"number\",\"minimum\":0},"
                "\"endSeconds\":{\"type\":\"number\",\"minimum\":0}", TARGET_REQUIRED),
            .output_schema = "{\"type\":\"object\"}",
            .handler = cue, .userdata = runtime
        },
        {
            .name = "deck_command_status",
            .description = "Read a transient operation receipt by idempotency key. Queued and accepted are separate from applied.",
            .input_schema = SCHEMA(ACTION_FIELDS, "\"sessionId\",\"key\""),
            .output_schema = "{\"type\":\"object\"}",
            .handler = command_status, .userdata = runtime
        },
        {
            .name = "deck_cancel_command",
            .description = "Request cancellation of one transient operation. Poll the receipt for settlement.",
            .input_schema = SCHEMA(ACTION_FIELDS, "\"sessionId\",\"key\""),
            .output_schema = "{\"type\":\"object\"}",
            .handler = cancel_command, .userdata = runtime
        }
    };
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; ++i) {
        int status = cmcp_server_add_tool(server, &tools[i]);
        if (status != CMCP_OK)
            return status;
    }
    return mcp_register_cue_batch(server, runtime);
}
