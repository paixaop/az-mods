/* SPDX-License-Identifier: GPL-3.0-only */
#include "http.h"
#include "providers.h"

#include <event2/buffer.h>
#include <event2/bufferevent_ssl.h>
#include <event2/event.h>
#include <event2/keyvalq_struct.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

struct gateway {
    struct mcp_runtime *runtime;
    const char *directory;
    struct mcp_config config;
    SSL_CTX *tls;
    struct event_base *base;
    struct evhttp *http;
    struct event *pulse;
    cmcp_server_t *mcp;
    struct mcp_providers *providers;
    struct mcp_pairing pairing;
    unsigned connections;
    bool setup_tls;
};

struct mcp_connection {
    struct gateway *gateway;
    struct bufferevent *bev;
    struct event *deadline;
};

static int connection_index = -1;
static pthread_once_t index_once = PTHREAD_ONCE_INIT;

static void release_connection(void *parent, void *ptr, CRYPTO_EX_DATA *data,
                               int index, long arg, void *argp)
{
    (void)parent;
    (void)data;
    (void)index;
    (void)arg;
    (void)argp;
    struct mcp_connection *connection = ptr;
    if (!connection)
        return;
    event_free(connection->deadline);
    --connection->gateway->connections;
    free(connection);
}

static void make_connection_index(void)
{
    connection_index = SSL_get_ex_new_index(0, NULL, NULL, NULL, release_connection);
}

static void expire_connection(evutil_socket_t socket, short what, void *user)
{
    (void)socket;
    (void)what;
    const struct mcp_connection *connection = user;
    evutil_socket_t fd = bufferevent_getfd(connection->bev);
    if (fd >= 0)
        shutdown(fd, SHUT_RDWR);
}

static bool attach_connection(SSL *ssl, struct gateway *gateway)
{
    const struct timeval maximum = { .tv_sec = 8 };
    struct mcp_connection *connection = calloc(1, sizeof *connection);
    if (!connection)
        return false;
    connection->gateway = gateway;
    connection->deadline = evtimer_new(gateway->base, expire_connection, connection);
    if (connection->deadline && evtimer_add(connection->deadline, &maximum) == 0 &&
        SSL_set_ex_data(ssl, connection_index, connection) == 1) {
        ++gateway->connections;
        return true;
    }
    if (connection->deadline)
        event_free(connection->deadline);
    free(connection);
    return false;
}

static struct bufferevent *new_tls(struct event_base *base, void *user)
{
    struct gateway *gateway = user;
    if (gateway->connections >= MCP_CONNECTION_LIMIT)
        return NULL;
    SSL *ssl = SSL_new(gateway->tls);
    if (!ssl)
        return NULL;
    if (attach_connection(ssl, gateway)) {
        struct mcp_connection *connection = SSL_get_ex_data(ssl, connection_index);
        struct bufferevent *bev = bufferevent_openssl_socket_new(
            base, -1, ssl, BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
        if (bev)
            connection->bev = bev;
        return bev;
    }
    SSL_free(ssl);
    return NULL;
}

static bool unique_headers(const struct evkeyvalq *headers)
{
    static const char *const single[] = {
        "Authorization", "Origin", "Content-Type", "MCP-Protocol-Version", "Mcp-Method", "Mcp-Name"
    };
    unsigned seen = 0;
    for (const struct evkeyval *header = headers->tqh_first; header; header = header->next.tqe_next) {
        for (unsigned i = 0; i < sizeof single / sizeof single[0]; ++i) {
            if (!strcasecmp(header->key, single[i])) {
                unsigned bit = 1u << i;
                if (seen & bit)
                    return false;
                seen |= bit;
                break;
            }
        }
    }
    return true;
}

static bool authorized(const char *authorization, const char *token)
{
    const size_t length = 2 * MCP_TOKEN_BYTES;
    return authorization && strlen(authorization) == 7 + length &&
           !strncasecmp(authorization, "Bearer ", 7) &&
           CRYPTO_memcmp(authorization + 7, token, length) == 0;
}

static int request_policy(struct evhttp_request *request, const struct mcp_config *config)
{
    struct evkeyvalq *headers = evhttp_request_get_input_headers(request);
    if (strcmp(evhttp_request_get_uri(request), "/mcp"))
        return 404;
    if (!unique_headers(headers))
        return 400;
    const char *origin = evhttp_find_header(headers, "Origin");
    if (origin && (!config->origin[0] || strcmp(origin, config->origin)))
        return 403;
    if (!authorized(evhttp_find_header(headers, "Authorization"), config->token))
        return 401;
    return evhttp_request_get_command(request) == EVHTTP_REQ_POST ? 200 : 405;
}

static bool is_json(const char *value)
{
    static const char type[] = "application/json";
    if (!value || strncasecmp(value, type, sizeof type - 1))
        return false;
    value += sizeof type - 1;
    value += strspn(value, " \t");
    return !*value || *value == ';';
}

static void send_reply(struct evhttp_request *request, int status, const char *wire)
{
    struct evkeyvalq *out = evhttp_request_get_output_headers(request);
    evhttp_add_header(out, "Cache-Control", "no-store");
    evhttp_add_header(out, "Connection", "close");
    if (status == 405)
        evhttp_add_header(out, "Allow", "POST");
    if (status == 401)
        evhttp_add_header(out, "WWW-Authenticate", "Bearer realm=\"Deck MCP\"");
    struct evbuffer *buffer = NULL;
    if (wire && strlen(wire) <= MCP_RESPONSE_LIMIT) {
        buffer = evbuffer_new();
        if (buffer && evbuffer_add(buffer, wire, strlen(wire)) == 0)
            evhttp_add_header(out, "Content-Type", "application/json");
        else
            status = 500;
    } else if (wire) {
        status = 500;
    }
    evhttp_send_reply(request, status, NULL, buffer);
    if (buffer)
        evbuffer_free(buffer);
}

static int handle_post(struct evhttp_request *request, struct gateway *gateway, char **wire)
{
    struct evkeyvalq *headers = evhttp_request_get_input_headers(request);
    if (!is_json(evhttp_find_header(headers, "Content-Type")))
        return 415;
    struct evbuffer *body = evhttp_request_get_input_buffer(request);
    size_t length = evbuffer_get_length(body);
    const unsigned char *data = length ? evbuffer_pullup(body, -1) : (const unsigned char *)"";
    if (!data)
        return 500;
    return mcp_http_exchange(gateway->mcp, headers, (const char *)data, length, wire);
}

static void handle_pair(struct evhttp_request *request, struct gateway *gateway)
{
    if (mcp_token_requested(gateway->runtime)) {
        send_reply(request, 404, NULL);
        return;
    }
    mcp_pair_poll(&gateway->pairing, gateway->runtime, &gateway->config);
    mcp_pair_http(&gateway->pairing, gateway->runtime, &gateway->config, request);
}

static void handle_http(struct evhttp_request *request, void *user)
{
    struct gateway *gateway = user;
    if (atomic_load(&gateway->runtime->stopping)) {
        send_reply(request, 503, NULL);
        return;
    }
    const char *path = evhttp_request_get_uri(request);
    if (!strcmp(path, MCP_PAIR_PATH)) {
        handle_pair(request, gateway);
        return;
    }
    char *wire = NULL;
    int status = request_policy(request, &gateway->config);
    if (status == 200)
        status = handle_post(request, gateway, &wire);
    send_reply(request, status, wire);
    free(wire);
}

static void poll_token(struct gateway *gateway)
{
    if (!mcp_token_requested(gateway->runtime))
        return;
    mcp_pair_cancel(&gateway->pairing, gateway->runtime);
    char replacement[MCP_SECRET_SIZE];
    bool saved = mcp_generate_secret(replacement) && mcp_replace_token(gateway->directory, replacement);
    if (saved) {
        memcpy(gateway->config.token, replacement, sizeof replacement);
        mcp_pair_start(&gateway->pairing, gateway->runtime, &gateway->config);
    }
    mcp_token_publish(gateway->runtime, saved ? MCP_TOKEN_SAVED : MCP_TOKEN_FAILED,
                      saved ? gateway->config.token : NULL);
    OPENSSL_cleanse(replacement, sizeof replacement);
}

static void poll_stop(evutil_socket_t socket, short what, void *user)
{
    (void)socket;
    (void)what;
    struct gateway *gateway = user;
    if (atomic_load(&gateway->runtime->stopping))
        event_base_loopbreak(gateway->base);
    else {
        poll_token(gateway);
        mcp_pair_poll(&gateway->pairing, gateway->runtime, &gateway->config);
        mcp_providers_sweep(gateway->providers);
    }
}

static bool open_http(struct gateway *gateway)
{
    const struct timeval interval = { .tv_usec = 50000 };
    gateway->base = event_base_new();
    if (!gateway->base)
        return false;
    gateway->http = evhttp_new(gateway->base);
    if (!gateway->http)
        return false;
    evhttp_set_bevcb(gateway->http, new_tls, gateway);
    evhttp_set_gencb(gateway->http, handle_http, gateway);
    evhttp_set_allowed_methods(gateway->http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_DELETE |
                                               EVHTTP_REQ_HEAD | EVHTTP_REQ_PUT | EVHTTP_REQ_OPTIONS |
                                               EVHTTP_REQ_PATCH);
    evhttp_set_max_headers_size(gateway->http, 8192);
    evhttp_set_max_body_size(gateway->http, MCP_BODY_LIMIT);
    evhttp_set_timeout(gateway->http, 4);
    gateway->pulse = event_new(gateway->base, -1, EV_PERSIST, poll_stop, gateway);
    return gateway->pulse && event_add(gateway->pulse, &interval) == 0 &&
           evhttp_bind_socket(gateway->http, gateway->config.bind, gateway->config.port) == 0;
}

static bool open_providers(struct gateway *gateway)
{
    if (!gateway->config.provider_count)
        return true;
    gateway->providers = mcp_providers_start(gateway->base, gateway->mcp, gateway->runtime, &gateway->config);
    return gateway->providers != NULL;
}
static bool open_tls_runtime(struct gateway *gateway, const char *directory)
{
    if (pthread_once(&index_once, make_connection_index) || connection_index < 0)
        return false;
    gateway->tls = gateway->setup_tls ? mcp_setup_tls(directory, gateway->config.bind) : mcp_load_tls(directory);
    return gateway->tls && mcp_generate_secret(gateway->runtime->session);
}

static bool open_gateway(struct gateway *gateway, const char *directory)
{
    if (!open_tls_runtime(gateway, directory))
        return false;
    gateway->mcp = cmcp_server_new("Deck MCP", MCP_VERSION);
    if (!gateway->mcp || mcp_register_tools(gateway->mcp, gateway->runtime) != CMCP_OK ||
        !open_http(gateway))
        return false;
    return open_providers(gateway);
}

static void close_gateway(struct gateway *gateway)
{
    if (gateway->pulse)
        event_free(gateway->pulse);
    if (gateway->http)
        evhttp_free(gateway->http);
    mcp_providers_stop(gateway->providers);
    if (gateway->base)
        event_base_free(gateway->base);
    cmcp_server_free(gateway->mcp);
    mcp_pair_close(&gateway->pairing);
    SSL_CTX_free(gateway->tls);
    OPENSSL_cleanse(&gateway->config, sizeof gateway->config);
}

static bool block_sigpipe(void)
{
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    return pthread_sigmask(SIG_BLOCK, &blocked, NULL) == 0;
}

static bool prepare_lan(struct gateway *gateway, const char *directory)
{
    if (strcmp(gateway->config.bind, "auto"))
        return true;
    gateway->setup_tls = mcp_file_missing(directory, "server.crt") && mcp_file_missing(directory, "server.key");
    mcp_connection_publish(gateway->runtime, NULL, 0, "Waiting for a LAN IPv4 address...");
    const struct timespec pause = { .tv_nsec = 200000000L };
    while (!atomic_load(&gateway->runtime->stopping)) {
        if (mcp_lan_address(gateway->config.bind))
            return true;
        nanosleep(&pause, NULL);
    }
    return false;
}

static void startup_error(struct mcp_runtime *runtime, const char *detail)
{
    mcp_connection_publish(runtime, NULL, 0, detail);
    runtime->core->log(MCP_ID, "%s", detail);
}

static void run_gateway(struct gateway *gateway)
{
    const az_core_v1 *core = gateway->runtime->core;
    mcp_connection_publish(gateway->runtime, NULL, 0, "Preparing Deck MCP...");
    const char *directory = core->data_dir(MCP_ID);
    if (!directory || OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG | OPENSSL_INIT_NO_ATEXIT, NULL) != 1 ||
        !mcp_prepare_config(directory, &gateway->config)) {
        startup_error(gateway->runtime, "Cannot set up Deck MCP. Check the installation USB.");
        return;
    }
    if (!prepare_lan(gateway, directory))
        return;
    if (!block_sigpipe() || !open_gateway(gateway, directory)) {
        startup_error(gateway->runtime, "HTTPS could not start. Check the network, clock and TLS files.");
        return;
    }
    gateway->directory = directory;
    mcp_pair_init(&gateway->pairing, gateway->tls, directory, gateway->setup_tls);
    mcp_pair_offer(gateway->runtime, MCP_PAIR_IDLE, NULL, NULL, NULL);
    mcp_connection_publish(gateway->runtime, gateway->config.bind, gateway->config.port,
                           gateway->setup_tls ? "CA file on the USB: " MCP_SETUP_CA : NULL);
    mcp_token_publish(gateway->runtime, MCP_TOKEN_READY, gateway->config.token);
    core->log(MCP_ID, "Serving MCP HTTPS on %s:%u", gateway->config.bind, gateway->config.port);
    if (!atomic_load(&gateway->runtime->stopping))
        event_base_dispatch(gateway->base);
}

void *mcp_serve(void *arg)
{
    struct mcp_runtime *runtime = arg;
    struct gateway gateway = { .runtime = runtime };
    run_gateway(&gateway);
    close_gateway(&gateway);
    mcp_pair_offer(runtime, MCP_PAIR_UNAVAILABLE, NULL, NULL, NULL);
    mcp_token_publish(runtime, MCP_TOKEN_UNAVAILABLE, NULL);
    if (atomic_load(&runtime->stopping))
        mcp_connection_publish(runtime, NULL, 0, "Deck MCP is stopped.");
    atomic_store(&runtime->working, false);
    return NULL;
}
