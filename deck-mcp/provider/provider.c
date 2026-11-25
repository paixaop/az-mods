/* SPDX-License-Identifier: GPL-3.0-only */
#include "client-wire.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


enum call_phase { IDLE, READY, RUNNING, FINISHING, COMPLETE };
#define PHASE_BITS 3
#define PHASE_MASK ((1u << PHASE_BITS) - 1u)

struct az_mcp_provider {
    const char *mod_id;
    const az_mcp_tool_v1 *tools;
    size_t count;
    atomic_uint_fast64_t lease;
    atomic_uint_fast64_t call_state;
    atomic_bool stopping, working;
    uint64_t sequence, remote_id, deadline;
    const az_mcp_tool_v1 *tool;
    cmcp_json_t *args, *result;
    int status;
};

static az_mcp_provider local_provider;

static uint_fast64_t call_state(uint64_t id, enum call_phase phase)
{
    return (id << PHASE_BITS) | (uint_fast64_t)phase;
}

static enum call_phase phase_of(uint_fast64_t state)
{
    return (enum call_phase)(state & PHASE_MASK);
}

static bool put_string(cmcp_json_t *object, const char *key, const char *string)
{
    return az_mcp_set(object, key, cmcp_json_new_string(string ? string : "")) == 0;
}

static bool put_number(cmcp_json_t *object, const char *key, uint64_t number)
{
    return az_mcp_set(object, key, cmcp_json_new_int((long long)number)) == 0;
}

static cmcp_json_t *tool_descriptor(const az_mcp_tool_v1 *tool)
{
    cmcp_json_t *out = cmcp_json_new_object();
    if (put_string(out, "name", tool->name) && put_string(out, "title", tool->title) &&
        put_string(out, "description", tool->description) && put_string(out, "input", tool->input_schema) &&
        put_string(out, "output", tool->output_schema) && put_number(out, "grant", tool->grant))
        return out;
    cmcp_json_free(out);
    return NULL;
}

static cmcp_json_t *tool_descriptors(const az_mcp_provider *provider)
{
    cmcp_json_t *list = cmcp_json_new_array();
    for (size_t i = 0; i < provider->count; ++i) {
        cmcp_json_t *tool = tool_descriptor(&provider->tools[i]);
        if (cmcp_json_array_append(list, tool) != 0) {
            cmcp_json_free(tool);
            cmcp_json_free(list);
            return NULL;
        }
    }
    return list;
}

static cmcp_json_t *registration(const az_mcp_provider *provider)
{
    cmcp_json_t *request = az_mcp_message("register");
    if (put_number(request, "version", AZ_MCP_PROVIDER_VERSION) &&
        put_string(request, "id", provider->mod_id) &&
        az_mcp_set(request, "tools", tool_descriptors(provider)) == 0)
        return request;
    cmcp_json_free(request);
    return NULL;
}

static bool message_is(const cmcp_json_t *message, const char *expected)
{
    const char *type = az_mcp_string(message, "type");
    return type && !strcmp(type, expected);
}

static bool expect_registered(int fd, const az_mcp_provider *provider)
{
    cmcp_json_t *message = registration(provider);
    bool sent = message && az_mcp_send(fd, message);
    cmcp_json_free(message);
    if (!sent)
        return false;
    cmcp_json_t *reply = az_mcp_receive(fd);
    bool accepted = message_is(reply, "registered");
    cmcp_json_free(reply);
    return accepted;
}

static bool leased(const az_mcp_provider *provider)
{
    return !atomic_load(&provider->stopping) &&
           az_mcp_before(az_mcp_clock(), atomic_load(&provider->lease));
}

static void delay(long ms)
{
    const struct timespec wait = { .tv_nsec = ms * 1000000L };
    nanosleep(&wait, NULL);
}

static void clear_call(az_mcp_provider *provider)
{
    cmcp_json_free(provider->args);
    cmcp_json_free(provider->result);
    provider->args = NULL;
    provider->result = NULL;
    atomic_store(&provider->call_state, call_state(provider->sequence, IDLE));
}

static bool clear_unused(az_mcp_provider *provider)
{
    uint_fast64_t state = atomic_load(&provider->call_state);
    enum call_phase phase = phase_of(state);
    if (phase == IDLE)
        return true;
    if (phase == COMPLETE || (phase == READY && atomic_compare_exchange_strong(&provider->call_state, &state,
                                                                       call_state(provider->sequence, IDLE)))) {
        clear_call(provider);
        return true;
    }
    return false;
}

static const az_mcp_tool_v1 *find_tool(const az_mcp_provider *provider, const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < provider->count; ++i)
        if (strcmp(name, provider->tools[i].name) == 0)
            return &provider->tools[i];
    return NULL;
}

static bool read_call(az_mcp_provider *provider, const cmcp_json_t *message)
{
    uint64_t id = az_mcp_number(message, "id");
    uint64_t deadline = az_mcp_number(message, "deadline");
    const az_mcp_tool_v1 *tool = find_tool(provider, az_mcp_string(message, "action"));
    const cmcp_json_t *args = cmcp_json_object_get(message, "args");
    if (!id || !tool || !args || args->type != CMCP_JSON_OBJECT || provider->sequence == (UINT64_MAX >> PHASE_BITS))
        return false;
    provider->args = cmcp_json_clone(args);
    if (!provider->args)
        return false;
    ++provider->sequence;
    provider->remote_id = id;
    provider->deadline = deadline;
    provider->tool = tool;
    atomic_store(&provider->call_state, call_state(provider->sequence, READY));
    return true;
}

static cmcp_json_t *completion_message(az_mcp_provider *provider)
{
    cmcp_json_t *message = az_mcp_message("result");
    if (put_number(message, "id", provider->remote_id) &&
        az_mcp_set(message, "status", cmcp_json_new_int(provider->status)) == 0 &&
        az_mcp_set(message, "result", provider->result ? cmcp_json_clone(provider->result)
                                                      : cmcp_json_new_object()) == 0)
        return message;
    cmcp_json_free(message);
    return NULL;
}

static bool send_completion(int fd, az_mcp_provider *provider)
{
    while (leased(provider) && az_mcp_before(az_mcp_clock(), provider->deadline)) {
        if (phase_of(atomic_load(&provider->call_state)) == COMPLETE) {
            cmcp_json_t *message = completion_message(provider);
            bool ok = message && az_mcp_send(fd, message);
            cmcp_json_free(message);
            clear_call(provider);
            return ok;
        }
        delay(10);
    }
    clear_unused(provider);
    return false;
}

static bool poll_call(int fd, az_mcp_provider *provider, const cmcp_json_t *lease)
{
    if (!az_mcp_send(fd, lease))
        return false;
    cmcp_json_t *message = az_mcp_receive(fd);
    bool idle = message_is(message, "idle");
    bool call = !idle && message_is(message, "call") && read_call(provider, message);
    cmcp_json_free(message);
    return idle || (call && send_completion(fd, provider));
}

static void serve_connection(int fd, az_mcp_provider *provider)
{
    if (!expect_registered(fd, provider))
        return;
    cmcp_json_t *lease = az_mcp_message("lease");
    if (!lease)
        return;
    while (leased(provider) && poll_call(fd, provider, lease))
        delay(50);
    cmcp_json_free(lease);
}

static bool await_first_tick(const az_mcp_provider *provider)
{
    uint64_t deadline = az_mcp_clock() + AZ_MCP_LEASE_NS;
    while (!atomic_load(&provider->stopping) && az_mcp_before(az_mcp_clock(), deadline)) {
        if (leased(provider))
            return true;
        delay(50);
    }
    return false;
}

static void *provider_worker(void *user)
{
    az_mcp_provider *provider = user;
    bool started = await_first_tick(provider);
    while (started && leased(provider)) {
        if (clear_unused(provider)) {
            int fd = az_mcp_connect();
            if (fd >= 0) {
                serve_connection(fd, provider);
                close(fd);
            }
        }
        delay(100);
    }
    clear_unused(provider);
    atomic_store(&provider->working, false);
    return NULL;
}

static bool valid_tool(const az_mcp_tool_v1 *tool)
{
    return tool->size >= sizeof *tool && az_mcp_name(tool->name) && tool->invoke &&
           tool->input_schema && tool->grant >= AZ_MCP_READ && tool->grant <= AZ_MCP_SAVED;
}

static bool valid_tools(const az_mcp_tool_v1 *tools, size_t count)
{
    if (!tools || !count || count > AZ_MCP_PROVIDER_MAX_TOOLS)
        return false;
    for (size_t i = 0; i < count; ++i)
        if (!valid_tool(&tools[i]))
            return false;
    return true;
}

static bool acquire_provider(az_mcp_provider *provider)
{
    if (atomic_exchange(&provider->working, true))
        return false;
    if (clear_unused(provider))
        return true;
    atomic_store(&provider->working, false);
    return false;
}

int az_mcp_register_tools(const az_core_v1 *core, const char *mod_id,
                          const az_mcp_tool_v1 *tools, size_t count,
                          az_mcp_provider **out)
{
    if (!out || !AZ_HAS(core, thread_start) || !az_mcp_name(mod_id) || !valid_tools(tools, count))
        return AZ_INVALID;
    *out = NULL;
    az_mcp_provider *provider = &local_provider;
    if (!acquire_provider(provider))
        return AZ_BUSY;
    atomic_store(&provider->stopping, false);
    atomic_store(&provider->lease, 0);
    provider->mod_id = mod_id;
    provider->tools = tools;
    provider->count = count;
    if (core->thread_start(mod_id, "mcp-provider", provider_worker, provider)) {
        atomic_store(&provider->working, false);
        return AZ_FAILED;
    }
    *out = provider;
    return AZ_OK;
}

int az_mcp_provider_complete(az_mcp_provider *provider, uint64_t id, int status, cmcp_json_t *result)
{
    uint_fast64_t state = call_state(id, RUNNING);
    if (!provider || id > (UINT64_MAX >> PHASE_BITS) ||
        !atomic_compare_exchange_strong(&provider->call_state, &state, call_state(id, FINISHING)))
        return AZ_STALE;
    provider->status = status;
    provider->result = result;
    atomic_store(&provider->call_state, call_state(id, COMPLETE));
    return AZ_OK;
}

static void invoke(az_mcp_provider *provider)
{
    cmcp_json_t *result = NULL;
    uint64_t id = provider->sequence;
    int status = AZ_STALE;
    const az_mcp_tool_v1 *tool = provider->tool;
    if (az_mcp_before(az_mcp_clock(), provider->deadline))
        status = tool->invoke(provider->args, id, provider->deadline, &result, tool->user);
    if (status != AZ_PENDING && az_mcp_provider_complete(provider, id, status, result) == AZ_OK)
        return;
    cmcp_json_free(result);
}

void az_mcp_provider_tick(az_mcp_provider *provider)
{
    if (!provider || atomic_load(&provider->stopping))
        return;
    uint64_t now = az_mcp_clock();
    if (!now)
        return;
    atomic_store(&provider->lease, now + AZ_MCP_LEASE_NS);
    uint_fast64_t state = atomic_load(&provider->call_state);
    if (phase_of(state) == READY && atomic_compare_exchange_strong(&provider->call_state, &state,
            (state & ~(uint_fast64_t)PHASE_MASK) | RUNNING))
        invoke(provider);
}

void az_mcp_provider_stop(az_mcp_provider *provider)
{
    if (provider)
        atomic_store(&provider->stopping, true);
}
