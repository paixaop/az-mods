/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"
#include "provider/wire.h"

#include <stdio.h>
#include <string.h>

static bool saved_grant(const struct mcp_runtime *runtime)
{
    return az_mcp_before(az_mcp_clock(), atomic_load(&runtime->saved_edits));
}

static void observe_settings(struct mcp_runtime *runtime)
{
    struct mcp_settings *settings = &runtime->settings;
    settings->count = 0;
    for (unsigned index = 0; index < MCP_SETTINGS_MAX; ++index) {
        az_official_setting_v1 value = { .size = sizeof value };
        int status = runtime->core->official_setting_read(index, &value);
        if (status == AZ_NOT_FOUND)
            return;
        if (status == AZ_OK && value.version == AZ_DATA_VERSION)
            settings->observed[settings->count++] = value;
    }
}

static int setting_apply(struct mcp_runtime *runtime, const struct mcp_setting_change *job)
{
    if (!saved_grant(runtime))
        return AZ_CANCELLED;
    if (!az_mcp_before(az_mcp_clock(), job->deadline))
        return AZ_STALE;
    return runtime->core->official_setting_write(MCP_ID, job->setting, job->expected, job->value);
}

static bool run_write(struct mcp_runtime *runtime)
{
    struct mcp_settings *settings = &runtime->settings;
    for (size_t i = 0; i < settings->used; ++i) {
        struct mcp_setting_change *change = &settings->writes[i];
        if (change->status == AZ_PENDING) {
            change->status = setting_apply(runtime, change);
            return true;
        }
    }
    return false;
}

void mcp_settings_tick(struct mcp_runtime *runtime)
{
    if (!AZ_HAS(runtime->core, official_setting_read) || pthread_mutex_trylock(&runtime->lock))
        return;
    uint64_t now = az_mcp_clock();
    struct mcp_settings *settings = &runtime->settings;
    if (run_write(runtime) || !az_mcp_before(now, settings->next_read)) {
        observe_settings(runtime);
        settings->next_read = now + AZ_MCP_NS / 5;
    }
    pthread_mutex_unlock(&runtime->lock);
}

static cmcp_json_t *setting_json(const az_official_setting_v1 *value)
{
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "setting", cmcp_json_new_string(value->key));
    mcp_set(out, "label", cmcp_json_new_string(value->label));
    mcp_set(out, "configured", cmcp_json_new_int(value->configured));
    mcp_set(out, "effective", cmcp_json_new_int(value->effective));
    mcp_set(out, "available", cmcp_json_new_bool(value->available));
    mcp_set(out, "choices", cmcp_json_new_int(value->choices));
    mcp_set(out, "kind", cmcp_json_new_string(value->kind == AZ_ROW_CHOICE ? "choice" : "toggle"));
    return out;
}

static cmcp_json_t *seen_settings(struct mcp_runtime *runtime, const char *key)
{
    az_official_setting_v1 copy[MCP_SETTINGS_MAX];
    pthread_mutex_lock(&runtime->lock);
    size_t count = runtime->settings.count;
    memcpy(copy, runtime->settings.observed, count * sizeof copy[0]);
    pthread_mutex_unlock(&runtime->lock);
    cmcp_json_t *list = cmcp_json_new_array();
    for (size_t i = 0; i < count; ++i) {
        if (key && strcmp(key, copy[i].key))
            continue;
        cmcp_json_t *entry = setting_json(&copy[i]);
        if (cmcp_json_array_append(list, entry))
            cmcp_json_free(entry);
    }
    return list;
}

static bool valid_text(const char *value, size_t capacity)
{
    return value && value[0] && strlen(value) < capacity;
}

static int setting_int(const cmcp_json_t *args, const char *key)
{
    const cmcp_json_t *value = cmcp_json_object_get(args, key);
    return value && value->type == CMCP_JSON_INT && value->i >= 0 && value->i <= 255
        ? (int)value->i : -1;
}

static bool parse_change(const cmcp_json_t *args, struct mcp_setting_change *change)
{
    const char *setting = az_mcp_string(args, "setting");
    const char *key = az_mcp_string(args, "key");
    if (!valid_text(key, sizeof change->key) || !valid_text(setting, sizeof change->setting))
        return false;
    change->value = setting_int(args, "configured");
    change->expected = setting_int(args, "expected");
    if (change->value < 0 || change->expected < 0)
        return false;
    snprintf(change->key, sizeof change->key, "%s", key);
    snprintf(change->setting, sizeof change->setting, "%s", setting);
    return true;
}

static bool same_change(const struct mcp_setting_change *a, const struct mcp_setting_change *b)
{
    return !strcmp(a->setting, b->setting) && a->expected == b->expected && a->value == b->value;
}

static struct mcp_setting_change *find_change(struct mcp_settings *settings, const char *key)
{
    for (size_t i = 0; i < settings->used; ++i)
        if (!strcmp(settings->writes[i].key, key))
            return &settings->writes[i];
    return NULL;
}

static int queue_setting(struct mcp_runtime *runtime, struct mcp_setting_change *change)
{
    struct mcp_settings *settings = &runtime->settings;
    struct mcp_setting_change *prior = find_change(settings, change->key);
    if (prior) {
        if (!same_change(prior, change))
            return AZ_INVALID;
        *change = *prior;
        return AZ_OK;
    }
    if (!saved_grant(runtime))
        return AZ_CANCELLED;
    if (!AZ_HAS(runtime->core, official_setting_write))
        return AZ_UNSUPPORTED;
    if (settings->used == MCP_SETTING_WRITES)
        return AZ_LIMIT;
    change->status = AZ_PENDING;
    change->deadline = az_mcp_clock() + AZ_MCP_CALL_NS;
    settings->writes[settings->used++] = *change;
    return AZ_OK;
}

static int setting_command(struct mcp_runtime *runtime, const cmcp_json_t *args,
                           const char *action, struct mcp_setting_change *change)
{
    const char *session = az_mcp_string(args, "sessionId");
    const char *key = az_mcp_string(args, "key");
    if (!session || strcmp(session, runtime->session) || !valid_text(key, sizeof change->key))
        return AZ_INVALID;
    if (!strcmp(action, "write")) {
        if (!parse_change(args, change))
            return AZ_INVALID;
        return queue_setting(runtime, change);
    }
    struct mcp_setting_change *prior = find_change(&runtime->settings, key);
    if (!prior)
        return AZ_NOT_FOUND;
    *change = *prior;
    return AZ_OK;
}

static cmcp_json_t *setting_requests(struct mcp_runtime *runtime, const cmcp_json_t *args,
                                     const char *action)
{
    struct mcp_setting_change change = {0};
    pthread_mutex_lock(&runtime->lock);
    int status = setting_command(runtime, args, action, &change);
    pthread_mutex_unlock(&runtime->lock);
    cmcp_json_t *out = mcp_session_result(runtime);
    mcp_set(out, "status", cmcp_json_new_int(status == AZ_OK ? change.status : status));
    if (status == AZ_OK) {
        mcp_set(out, "key", cmcp_json_new_string(change.key));
        mcp_set(out, "setting", cmcp_json_new_string(change.setting));
        mcp_set(out, "configured", cmcp_json_new_int(change.value));
    }
    return out;
}

static int settings_tool(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                         cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    const char *action = az_mcp_string(args, "action");
    if (!action)
        return mcp_tool_error("Invalid settings action.", content, is_error);
    if (!strcmp(action, "write") || !strcmp(action, "status"))
        return mcp_tool_result(ctx, setting_requests(runtime, args, action), content);
    const char *key = az_mcp_string(args, "setting");
    if (!strcmp(action, "read") && !key)
        return mcp_tool_error("A setting key is required.", content, is_error);
    cmcp_json_t *out = mcp_session_result(runtime);
    mcp_set(out, "settings", seen_settings(runtime, key));
    if (AZ_HAS(runtime->core, ui_theme))
        mcp_set(out, "theme", cmcp_json_new_int(runtime->core->ui_theme()));
    return mcp_tool_result(ctx, out, content);
}

static const char *screen_name(int screen)
{
    const char *const names[] = { "other", "source", "browse", "search", "deck", "menu" };
    return screen >= 0 && (size_t)screen < sizeof names / sizeof names[0] ? names[screen] : names[0];
}

static int ui_state(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                    cmcp_json_t **content, int *is_error)
{
    (void)args;
    (void)is_error;
    struct mcp_runtime *runtime = user;
    const az_core_v1 *core = runtime->core;
    cmcp_json_t *out = mcp_session_result(runtime);
    mcp_set(out, "view", cmcp_json_new_string(screen_name(core->view())));
    if (AZ_HAS(core, deck_count))
        mcp_set(out, "visibleDecks", cmcp_json_new_int(core->deck_count()));
    return mcp_tool_result(ctx, out, content);
}

int mcp_register_settings_tools(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    const cmcp_tool_t ui = { .name = "deck_ui_state", .description = "Read the current deck screen category.",
        .input_schema = "{\"type\":\"object\",\"additionalProperties\":false}", .handler = ui_state, .userdata = runtime };
    const cmcp_tool_t settings = {
        .name = "deck_azmods_settings", .description = "List, read or request a saved edit to an official Mod Menu setting.",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"list\",\"read\",\"write\",\"status\"]},"
            "\"setting\":{\"type\":\"string\",\"maxLength\":31},\"sessionId\":{\"type\":\"string\",\"maxLength\":64},"
            "\"expected\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
            "\"configured\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
            "\"key\":{\"type\":\"string\",\"maxLength\":64}},\"required\":[\"action\"],\"additionalProperties\":false}",
        .handler = settings_tool, .userdata = runtime
    };
    if (AZ_HAS(runtime->core, view) && cmcp_server_add_tool(server, &ui) != CMCP_OK)
        return CMCP_EINVAL;
    if (AZ_HAS(runtime->core, official_setting_read))
        return cmcp_server_add_tool(server, &settings);
    return CMCP_OK;
}
