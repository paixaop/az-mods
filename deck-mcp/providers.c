/* SPDX-License-Identifier: GPL-3.0-only */
#include "providers.h"
#include "cmcp_schema.h"

#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROVIDER_JOBS 32
#define TOOL_NAME_BYTES (2 * AZ_MCP_NAME_LIMIT + 2)
enum provider_job_phase { PROVIDER_QUEUED, PROVIDER_SENT, PROVIDER_DONE };

struct provider_connection;
struct provider_tool {
    struct provider_connection *connection;
    enum az_mcp_grant grant;
    char name[TOOL_NAME_BYTES];
    cmcp_json_t *output_schema;
};

struct provider_job {
    struct provider_connection *connection;
    const struct provider_tool *tool;
    uint64_t id, deadline, collect_until;
    enum provider_job_phase phase;
    enum az_mcp_grant grant;
    unsigned generation;
    int status;
    bool uncertain;
    char key[MCP_KEY_SIZE], name[TOOL_NAME_BYTES];
    cmcp_json_t *args, *result;
};

struct mcp_providers {
    struct event_base *base;
    struct evconnlistener *listener;
    cmcp_server_t *server;
    struct mcp_runtime *runtime;
    const struct mcp_config *config;
    struct sockaddr_un address;
    bool bound;
    uint64_t sequence;
    struct provider_connection *connections[MCP_PROVIDER_LIMIT];
    struct provider_job jobs[PROVIDER_JOBS];
};

struct provider_connection {
    struct mcp_providers *providers;
    struct bufferevent *bev;
    struct provider_job *current;
    uint64_t lease;
    size_t slot, count;
    const struct mcp_provider_policy *policy;
    struct provider_tool tools[AZ_MCP_PROVIDER_MAX_TOOLS];
};

static const struct mcp_provider_policy *policy_for(const struct mcp_config *config, const char *id)
{
    if (!az_mcp_name(id))
        return NULL;
    for (size_t i = 0; i < config->provider_count; ++i)
        if (!strcmp(config->providers[i].id, id))
            return &config->providers[i];
    return NULL;
}

static unsigned local_grant(const struct mcp_runtime *runtime, enum az_mcp_grant grant)
{
    switch (grant) {
    case AZ_MCP_READ: return 1;
    case AZ_MCP_LIVE: return mcp_live_grant_generation(runtime, az_mcp_clock());
    case AZ_MCP_SAVED: return mcp_saved_grant_generation(runtime, az_mcp_clock());
    }
    return 0;
}

static void finish_job(struct provider_job *job, int status, bool uncertain)
{
    job->phase = PROVIDER_DONE;
    job->status = status;
    job->uncertain = uncertain;
    job->collect_until = mcp_receipt_deadline(az_mcp_clock());
    job->tool = NULL;
    job->connection = NULL;
}

static void forget_job(struct provider_job *job)
{
    cmcp_json_free(job->args);
    cmcp_json_free(job->result);
    *job = (struct provider_job){0};
}

static struct provider_job *free_job(struct mcp_providers *providers)
{
    for (size_t i = 0; i < PROVIDER_JOBS; ++i)
        if (!providers->jobs[i].id)
            return &providers->jobs[i];
    return NULL;
}

static bool valid_key(const char *key)
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-";
    size_t length = key ? strlen(key) : 0;
    return length && length < MCP_KEY_SIZE && strspn(key, alphabet) == length;
}

static struct provider_job *find_retry(struct mcp_providers *providers, const char *key)
{
    for (size_t i = 0; i < PROVIDER_JOBS; ++i)
        if (!strcmp(providers->jobs[i].key, key))
            return &providers->jobs[i];
    return NULL;
}

static cmcp_json_t *job_json(const struct mcp_providers *providers, const struct provider_job *job)
{
    static const char *const phase[] = { "queued", "accepted", "complete" };
    cmcp_json_t *out = mcp_session_result(providers->runtime);
    mcp_set(out, "operationId", mcp_id(job->id));
    mcp_set(out, "state", cmcp_json_new_string(job->uncertain ? "uncertain" : phase[job->phase]));
    mcp_set(out, "status", cmcp_json_new_int(job->status));
    if (job->result)
        mcp_set(out, "result", cmcp_json_clone(job->result));
    return out;
}

static struct provider_job *queue_job(struct provider_tool *tool, const cmcp_json_t *args,
                                      const char *key, unsigned generation)
{
    struct provider_connection *connection = tool->connection;
    struct mcp_providers *providers = connection->providers;
    struct provider_job *job = free_job(providers);
    if (!job || providers->sequence >= LLONG_MAX)
        return NULL;
    cmcp_json_t *copy = cmcp_json_clone(args);
    if (!copy)
        return NULL;
    *job = (struct provider_job){ .connection = connection, .tool = tool, .args = copy,
        .grant = tool->grant, .generation = generation, .id = ++providers->sequence, .status = AZ_PENDING,
        .deadline = az_mcp_clock() + AZ_MCP_CALL_NS };
    snprintf(job->name, sizeof job->name, "%s", tool->name);
    if (key)
        snprintf(job->key, sizeof job->key, "%s", key);
    return job;
}

static bool matches_retry(const struct provider_job *job, const struct provider_tool *tool,
                          const cmcp_json_t *args)
{
    return !strcmp(job->name, tool->name) && cmcp_json_equal(job->args, args);
}

static int invoke_tool(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                        cmcp_json_t **content, int *is_error)
{
    struct provider_tool *tool = user;
    struct mcp_providers *providers = tool->connection->providers;
    const char *key = tool->grant == AZ_MCP_READ ? NULL : az_mcp_string(args, "key");
    if (tool->grant != AZ_MCP_READ && !valid_key(key))
        return mcp_tool_error("A valid idempotency key is required.", content, is_error);
    struct provider_job *prior = key ? find_retry(providers, key) : NULL;
    if (prior) {
        if (!matches_retry(prior, tool, args))
            return mcp_tool_error("This provider key already belongs to a different request.", content, is_error);
        return mcp_tool_result(ctx, job_json(providers, prior), content);
    }
    unsigned generation = local_grant(providers->runtime, tool->grant);
    if (!generation)
        return mcp_tool_error("This action requires a local control grant.", content, is_error);
    struct provider_job *job = queue_job(tool, args, key, generation);
    return job ? mcp_tool_result(ctx, job_json(providers, job), content)
               : mcp_tool_error("The provider operation ledger is full.", content, is_error);
}

static uint64_t operation_id(const cmcp_json_t *args)
{
    const char *text = az_mcp_string(args, "operationId");
    if (!text || strspn(text, "0123456789") != strlen(text))
        return 0;
    errno = 0;
    uint64_t id = strtoull(text, NULL, 10);
    return errno ? 0 : id;
}

static const struct provider_job *find_id(const struct mcp_providers *providers, uint64_t id)
{
    if (!id)
        return NULL;
    for (size_t i = 0; i < PROVIDER_JOBS; ++i)
        if (providers->jobs[i].id == id)
            return &providers->jobs[i];
    return NULL;
}

static int provider_status(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                            cmcp_json_t **content, int *is_error)
{
    struct mcp_providers *providers = user;
    const char *session = az_mcp_string(args, "sessionId");
    const struct provider_job *job = find_id(providers, operation_id(args));
    if (!session || strcmp(session, providers->runtime->session) || !job)
        return mcp_tool_error("Provider operation not found in this session.", content, is_error);
    return mcp_tool_result(ctx, job_json(providers, job), content);
}

static bool send_message(struct provider_connection *connection, cmcp_json_t *message)
{
    bool writable = evbuffer_get_length(bufferevent_get_output(connection->bev)) <= AZ_MCP_PROVIDER_FRAME;
    char *wire = message && writable ? cmcp_json_emit(message) : NULL;
    cmcp_json_free(message);
    if (!wire)
        return false;
    size_t length = strlen(wire);
    uint32_t header = htonl((uint32_t)length);
    bool sent = length <= AZ_MCP_PROVIDER_FRAME &&
                bufferevent_write(connection->bev, &header, sizeof header) == 0 &&
                bufferevent_write(connection->bev, wire, length) == 0;
    free(wire);
    return sent;
}

struct tool_definition {
    const char *name, *schema, *output;
    enum az_mcp_grant grant;
};

static bool parse_definition(const cmcp_json_t *item, struct tool_definition *out)
{
    out->name = az_mcp_string(item, "name");
    out->schema = az_mcp_string(item, "input");
    out->output = az_mcp_string(item, "output");
    const cmcp_json_t *grant = cmcp_json_object_get(item, "grant");
    if (!az_mcp_name(out->name) || !out->schema || !out->output || !grant || grant->type != CMCP_JSON_INT)
        return false;
    if (grant->i < AZ_MCP_READ || grant->i > AZ_MCP_SAVED)
        return false;
    out->grant = (enum az_mcp_grant)grant->i;
    return true;
}

static bool define_tool(struct provider_connection *connection, const cmcp_json_t *item)
{
    struct tool_definition definition;
    if (!parse_definition(item, &definition))
        return false;
    if (definition.grant > connection->policy->grant)
        return true;
    struct provider_tool *tool = &connection->tools[connection->count];
    tool->connection = connection;
    tool->grant = definition.grant;
    snprintf(tool->name, sizeof tool->name, "%s.%s", connection->policy->id, definition.name);
    tool->output_schema = *definition.output ? cmcp_json_parse_cstr(definition.output) : NULL;
    if (*definition.output && !tool->output_schema)
        return false;
    const cmcp_tool_t registration = {
        .name = tool->name, .title = az_mcp_string(item, "title"),
        .description = az_mcp_string(item, "description"), .input_schema = definition.schema,
        .output_schema = "{\"type\":\"object\"}", .handler = invoke_tool, .userdata = tool
    };
    if (cmcp_server_add_tool(connection->providers->server, &registration) == CMCP_OK) {
        ++connection->count;
        return true;
    }
    cmcp_json_free(tool->output_schema);
    tool->output_schema = NULL;
    return false;
}

static bool registered(const struct mcp_providers *providers, const struct mcp_provider_policy *policy)
{
    for (size_t i = 0; i < MCP_PROVIDER_LIMIT; ++i) {
        const struct provider_connection *other = providers->connections[i];
        if (other && other->policy == policy)
            return true;
    }
    return false;
}

static bool register_provider(struct provider_connection *connection, const cmcp_json_t *message)
{
    struct mcp_providers *providers = connection->providers;
    const struct mcp_provider_policy *policy = policy_for(providers->config, az_mcp_string(message, "id"));
    if (az_mcp_number(message, "version") != AZ_MCP_PROVIDER_VERSION || !policy || registered(providers, policy))
        return false;
    const cmcp_json_t *tools = cmcp_json_object_get(message, "tools");
    size_t count = cmcp_json_array_len(tools);
    if (!count || count > AZ_MCP_PROVIDER_MAX_TOOLS)
        return false;
    connection->policy = policy;
    for (size_t i = 0; i < count; ++i)
        if (!define_tool(connection, cmcp_json_array_at(tools, i)))
            return false;
    return send_message(connection, az_mcp_message("registered"));
}

static struct provider_job *take_job(struct provider_connection *connection)
{
    struct mcp_providers *providers = connection->providers;
    for (size_t i = 0; i < PROVIDER_JOBS; ++i) {
        struct provider_job *job = &providers->jobs[i];
        if (job->connection == connection && job->phase == PROVIDER_QUEUED)
            return job;
    }
    return NULL;
}

static cmcp_json_t *call_message(const struct provider_job *job)
{
    cmcp_json_t *message = az_mcp_message("call");
    const char *action = strchr(job->name, '.') + 1;
    if (az_mcp_set(message, "action", cmcp_json_new_string(action)) ||
        az_mcp_set(message, "id", cmcp_json_new_int((long long)job->id)) ||
        az_mcp_set(message, "deadline", cmcp_json_new_int((long long)job->deadline)) ||
        az_mcp_set(message, "args", cmcp_json_clone(job->args))) {
        cmcp_json_free(message);
        return NULL;
    }
    return message;
}

static bool send_call(struct provider_connection *connection, struct provider_job *job)
{
    if (local_grant(connection->providers->runtime, job->grant) != job->generation) {
        finish_job(job, AZ_CANCELLED, false);
        return send_message(connection, az_mcp_message("idle"));
    }
    if (!az_mcp_before(az_mcp_clock(), job->deadline)) {
        finish_job(job, AZ_STALE, false);
        return send_message(connection, az_mcp_message("idle"));
    }
    connection->current = job;
    job->phase = PROVIDER_SENT;
    return send_message(connection, call_message(job));
}

static bool result_status(const cmcp_json_t *node, int *status)
{
    if (!node || node->type != CMCP_JSON_INT || node->i > AZ_OK || node->i < AZ_NOT_FOUND)
        return false;
    *status = (int)node->i;
    return true;
}

static bool accept_result(struct provider_connection *connection, const cmcp_json_t *message)
{
    struct provider_job *job = connection->current;
    if (!job || az_mcp_number(message, "id") != job->id)
        return false;
    int status;
    const cmcp_json_t *result = cmcp_json_object_get(message, "result");
    if (!result_status(cmcp_json_object_get(message, "status"), &status))
        return false;
    bool valid = !job->tool->output_schema || status != AZ_OK ||
                 cmcp_schema_validate(job->tool->output_schema, result, NULL) == CMCP_OK;
    job->result = valid ? cmcp_json_clone(result) : NULL;
    connection->current = NULL;
    finish_job(job, valid ? status : AZ_INVALID, false);
    return true;
}

static bool receive_message(struct provider_connection *connection, const cmcp_json_t *message)
{
    const char *type = az_mcp_string(message, "type");
    if (!type)
        return false;
    connection->lease = az_mcp_clock() + AZ_MCP_LEASE_NS;
    if (!connection->policy)
        return !strcmp(type, "register") && register_provider(connection, message);
    if (!strcmp(type, "result"))
        return accept_result(connection, message);
    if (strcmp(type, "lease") || connection->current)
        return false;
    struct provider_job *job = take_job(connection);
    return job ? send_call(connection, job) : send_message(connection, az_mcp_message("idle"));
}

static void retire_connection(struct provider_connection *connection)
{
    struct mcp_providers *providers = connection->providers;
    for (size_t i = 0; i < PROVIDER_JOBS; ++i) {
        struct provider_job *job = &providers->jobs[i];
        if (job->connection == connection)
            finish_job(job, AZ_CANCELLED, job->phase == PROVIDER_SENT);
    }
    for (size_t i = 0; i < connection->count; ++i) {
        mcp_remove_tool(providers->server, connection->tools[i].name);
        cmcp_json_free(connection->tools[i].output_schema);
    }
    providers->connections[connection->slot] = NULL;
    bufferevent_free(connection->bev);
    free(connection);
}

static int receive_frame(struct provider_connection *connection)
{
    struct evbuffer *input = bufferevent_get_input(connection->bev);
    uint32_t header;
    if (evbuffer_copyout(input, &header, sizeof header) != sizeof header)
        return 0;
    size_t length = ntohl(header);
    if (!length || length > AZ_MCP_PROVIDER_FRAME)
        return -1;
    if (evbuffer_get_length(input) < sizeof header + length)
        return 0;
    evbuffer_drain(input, sizeof header);
    const char *wire = (const char *)evbuffer_pullup(input, (ev_ssize_t)length);
    cmcp_json_t *message = wire && mcp_utf8_prefix(wire, length) == length ? cmcp_json_parse(wire, length) : NULL;
    evbuffer_drain(input, length);
    bool received = message && receive_message(connection, message);
    cmcp_json_free(message);
    return received ? 1 : -1;
}

static void provider_read(struct bufferevent *bev, void *user)
{
    (void)bev;
    struct provider_connection *connection = user;
    int status;
    do {
        status = receive_frame(connection);
    } while (status > 0);
    if (status < 0)
        retire_connection(connection);
}

static void provider_event(struct bufferevent *bev, short what, void *user)
{
    (void)bev;
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
        retire_connection(user);
}

static bool add_connection(struct mcp_providers *providers, evutil_socket_t fd, size_t slot)
{
    struct provider_connection *connection = calloc(1, sizeof *connection);
    if (!connection)
        return false;
    connection->bev = bufferevent_socket_new(providers->base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!connection->bev) {
        free(connection);
        return false;
    }
    connection->providers = providers;
    connection->slot = slot;
    connection->lease = az_mcp_clock() + AZ_MCP_LEASE_NS;
    providers->connections[slot] = connection;
    bufferevent_setcb(connection->bev, provider_read, NULL, provider_event, connection);
    bufferevent_setwatermark(connection->bev, EV_READ, 0, AZ_MCP_PROVIDER_FRAME + sizeof(uint32_t));
    bufferevent_enable(connection->bev, EV_READ | EV_WRITE);
    return true;
}

static void accept_provider(struct evconnlistener *listener, evutil_socket_t fd,
                             struct sockaddr *address, int size, void *user)
{
    (void)listener;
    (void)address;
    (void)size;
    struct mcp_providers *providers = user;
    if (az_mcp_peer(fd)) {
        for (size_t i = 0; i < MCP_PROVIDER_LIMIT; ++i)
            if (!providers->connections[i] && add_connection(providers, fd, i))
                return;
    }
    evutil_closesocket(fd);
}

static void expire_connections(struct mcp_providers *providers, uint64_t now)
{
    for (size_t i = 0; i < MCP_PROVIDER_LIMIT; ++i) {
        struct provider_connection *conn = providers->connections[i];
        if (conn && !az_mcp_before(now, conn->lease))
            retire_connection(conn);
    }
}

static void expire_results(struct mcp_providers *providers, uint64_t now)
{
    for (size_t i = 0; i < PROVIDER_JOBS; ++i) {
        struct provider_job *job = &providers->jobs[i];
        if (job->id && job->phase == PROVIDER_DONE && mcp_receipt_expired(now, job->collect_until))
            forget_job(job);
    }
}
void mcp_providers_sweep(struct mcp_providers *providers)
{
    if (!providers)
        return;
    uint64_t now = az_mcp_clock();
    if (!now)
        return;
    expire_connections(providers, now);
    expire_results(providers, now);
}

struct mcp_providers *mcp_providers_start(struct event_base *base, cmcp_server_t *server,
                                          struct mcp_runtime *runtime, const struct mcp_config *config)
{
    struct mcp_providers *providers = calloc(1, sizeof *providers);
    if (!providers)
        return NULL;
    providers->base = base;
    providers->server = server;
    providers->runtime = runtime;
    providers->config = config;
    socklen_t size = az_mcp_address(&providers->address);
    providers->listener = evconnlistener_new_bind(base, accept_provider, providers,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC, MCP_PROVIDER_LIMIT,
        (const struct sockaddr *)&providers->address, size);
    providers->bound = providers->listener != NULL;
    const cmcp_tool_t status = {
        .name = "deck_provider_status", .description = "Read a deferred provider tool result.",
        .input_schema = "{\"type\":\"object\",\"properties\":{\"sessionId\":{\"type\":\"string\"},"
            "\"operationId\":{\"type\":\"string\"}},\"required\":[\"sessionId\",\"operationId\"],\"additionalProperties\":false}",
        .handler = provider_status, .userdata = providers
    };
    if (providers->listener && cmcp_server_add_tool(server, &status) == CMCP_OK)
        return providers;
    mcp_providers_stop(providers);
    return NULL;
}

void mcp_providers_stop(struct mcp_providers *providers)
{
    if (!providers)
        return;
    if (providers->listener)
        evconnlistener_free(providers->listener);
    if (providers->bound && providers->address.sun_path[0])
        unlink(providers->address.sun_path);
    for (size_t i = 0; i < MCP_PROVIDER_LIMIT; ++i)
        if (providers->connections[i])
            retire_connection(providers->connections[i]);
    for (size_t i = 0; i < PROVIDER_JOBS; ++i)
        forget_job(&providers->jobs[i]);
    free(providers);
}

void mcp_saved_arm(struct mcp_runtime *runtime)
{
    uint64_t now = az_mcp_clock();
    if (!now || az_mcp_before(now, atomic_load(&runtime->saved_edits)))
        return;
    if (atomic_load(&runtime->saved_generation) != UINT32_MAX) {
        atomic_fetch_add(&runtime->saved_generation, 1);
        atomic_store(&runtime->saved_edits, now + 900 * AZ_MCP_NS);
    }
}

void mcp_saved_disarm(struct mcp_runtime *runtime)
{
    atomic_store(&runtime->saved_edits, 0);
}

void mcp_provider_controls_init(struct mcp_runtime *runtime)
{
    atomic_store(&runtime->saved_edits, 0);
    atomic_store(&runtime->saved_generation, 0);
}
