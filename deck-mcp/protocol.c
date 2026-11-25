/* SPDX-License-Identifier: GPL-3.0-only */
#include "http.h"
#include "http_modern.h"
#include "cmcp.h"

#include <openssl/asn1.h>
#include <stdlib.h>
#include <string.h>

/* OpenSSL decodes each code point. This also lets us trim a copied SDK
 * string at a valid boundary when the fixed-size snapshot cuts a character. */
size_t mcp_utf8_prefix(const char *text, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        unsigned long codepoint;
        int bytes = UTF8_getc((const unsigned char *)text + offset, (int)(length - offset), &codepoint);
        if (bytes <= 0 || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
            break;
        offset += (size_t)bytes;
    }
    return offset;
}

static int rpc_error(const cmcp_id_t *id, int code, const char *message, char **wire)
{
    cmcp_rpc_message_t response;
    cmcp_rpc_message_init(&response);
    cmcp_rpc_make_error(&response, id, code, message, NULL);
    *wire = cmcp_rpc_emit(&response);
    cmcp_rpc_message_clear(&response);
    return *wire ? 400 : 500;
}

static bool string_usable(const cmcp_json_t *value)
{
    const char *str = cmcp_json_string(value);
    return !str || strlen(str) == cmcp_json_string_len(value);
}

static bool valid_routing_strings(const cmcp_json_t *body)
{
    const cmcp_json_t *params = cmcp_json_object_get(body, "params");
    const cmcp_json_t *meta = cmcp_json_object_get(params, "_meta");
    return string_usable(cmcp_json_object_get(body, "method")) &&
           string_usable(cmcp_json_object_get(params, "name")) &&
           string_usable(cmcp_json_object_get(params, "uri")) &&
           string_usable(cmcp_json_object_get(meta, "io.modelcontextprotocol/protocolVersion"));
}

static void routing_headers(cmcp_http_request_t *metadata, const struct evkeyvalq *headers)
{
    static char *const required[] = { "MCP-Protocol-Version", "Mcp-Method", "Mcp-Name" };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        const char *value = evhttp_find_header(headers, required[i]);
        if (value)
            metadata->headers[metadata->n_headers++] = (cmcp_http_header_t){
                .name = required[i], .value = (char *)value
            };
    }
}

static int validate_metadata(const cmcp_json_t *body, const struct evkeyvalq *headers,
                             const cmcp_rpc_message_t *request, bool *modern, char **wire)
{
    cmcp_http_request_t metadata = {0};
    routing_headers(&metadata, headers);
    const char *version = cmcp_http_header_get(&metadata, "MCP-Protocol-Version");
    *modern = cmcp_http_modern_is(body, &metadata);
    if (request->method && !strcmp(request->method, "server/discover"))
        *modern = true;
    if (*modern)
        return cmcp_http_modern_check(body, &metadata, wire);
    if (version && strcmp(version, CMCP_PROTOCOL_VERSION))
        return rpc_error(&request->id, CMCP_RPC_UNSUPPORTED_PROTOCOL_VERSION, "Unsupported MCP protocol version", wire);
    return 0;
}

static int dispatch(cmcp_server_t *server, const cmcp_rpc_message_t *request,
                    bool modern, char **wire)
{
    if (request->method && !strcmp(request->method, "subscriptions/listen")) {
        int status = rpc_error(&request->id, CMCP_RPC_METHOD_NOT_FOUND, "Subscriptions unavailable", wire);
        return status == 400 ? 404 : status;
    }
    int status = mcp_exchange(server, request, wire);
    if (status == 200 && modern)
        return cmcp_http_modern_status(*wire, strlen(*wire));
    return status;
}

static int parsed_exchange(cmcp_server_t *server, const struct evkeyvalq *headers,
                           const cmcp_json_t *body, cmcp_rpc_message_t *request, char **wire)
{
    if (valid_routing_strings(body) && cmcp_rpc_from_json(body, request) == CMCP_OK) {
        bool modern;
        int status = validate_metadata(body, headers, request, &modern, wire);
        return status ? status : dispatch(server, request, modern, wire);
    }
    const cmcp_id_t id = { .kind = CMCP_ID_NULL };
    return rpc_error(&id, CMCP_RPC_INVALID_REQUEST, "Invalid JSON-RPC message", wire);
}

int mcp_http_exchange(cmcp_server_t *server, const struct evkeyvalq *headers,
                      const char *body, size_t length, char **wire)
{
    *wire = NULL;
    cmcp_json_t *json = mcp_utf8_prefix(body, length) == length ? cmcp_json_parse(body, length) : NULL;
    if (!json) {
        const cmcp_id_t id = { .kind = CMCP_ID_NULL };
        return rpc_error(&id, CMCP_RPC_PARSE_ERROR, "Invalid JSON", wire);
    }
    cmcp_rpc_message_t request;
    cmcp_rpc_message_init(&request);
    int status = parsed_exchange(server, headers, json, &request, wire);
    cmcp_rpc_message_clear(&request);
    cmcp_json_free(json);
    return status;
}
