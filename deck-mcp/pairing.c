/* SPDX-License-Identifier: GPL-3.0-only */
#include "http.h"

#include <event2/buffer.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool certificate_fingerprint(X509 *cert, char fingerprint[MCP_FINGERPRINT_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char hash[32];
    unsigned length = 0;
    if (!cert || X509_digest(cert, EVP_sha256(), hash, &length) != 1 || length != sizeof hash)
        return false;
    for (unsigned i = 0; i < length; ++i) {
        fingerprint[2 * i] = hex[hash[i] >> 4];
        fingerprint[2 * i + 1] = hex[hash[i] & 15];
    }
    fingerprint[2 * length] = '\0';
    return true;
}

static bool self_signed(X509 *cert)
{
    if (X509_NAME_cmp(X509_get_subject_name(cert), X509_get_issuer_name(cert)) != 0)
        return false;
    EVP_PKEY *key = X509_get_pubkey(cert);
    bool verified = key && X509_verify(cert, key) == 1;
    EVP_PKEY_free(key);
    return verified;
}

static char *certificate_pem(X509 *cert)
{
    BIO *pem = BIO_new(BIO_s_mem());
    char *result = NULL;
    if (pem && PEM_write_bio_X509(pem, cert) == 1) {
        char *bytes = NULL;
        long n = BIO_get_mem_data(pem, &bytes);
        if (n > 0 && n < 16384)
            result = strndup(bytes, (size_t)n);
    }
    BIO_free(pem);
    return result;
}

static char *generated_ca(const char *directory)
{
    BIO *pem = mcp_open_pem(directory, MCP_SETUP_CA);
    X509 *cert = pem ? PEM_read_bio_X509(pem, NULL, NULL, NULL) : NULL;
    char *public_pem = cert ? certificate_pem(cert) : NULL;
    X509_free(cert);
    BIO_free(pem);
    return public_pem;
}

void mcp_pair_init(struct mcp_pairing *pair, SSL_CTX *tls, const char *directory, bool generated)
{
    X509 *leaf = SSL_CTX_get0_certificate(tls);
    if (!certificate_fingerprint(leaf, pair->fingerprint))
        return;
    if (generated) {
        pair->certificate = generated_ca(directory);
        if (!pair->certificate)
            pair->fingerprint[0] = '\0';
    } else if (self_signed(leaf)) {
        pair->certificate = certificate_pem(leaf);
        if (!pair->certificate)
            pair->fingerprint[0] = '\0';
    }
}

static void clear_ticket(struct mcp_pairing *pair)
{
    OPENSSL_cleanse(pair->code, sizeof pair->code);
    pair->expires = 0;
    pair->attempts = 0;
}

void mcp_pair_close(struct mcp_pairing *pair)
{
    clear_ticket(pair);
    free(pair->certificate);
}

void mcp_pair_cancel(struct mcp_pairing *pair, struct mcp_runtime *runtime)
{
    clear_ticket(pair);
    mcp_pair_offer(runtime, MCP_PAIR_IDLE, NULL, NULL, NULL);
}

static bool generate_code(char code[MCP_PAIR_CODE_SIZE], const char *previous)
{
    uint32_t range = 1;
    for (unsigned i = 0; i < MCP_PAIR_CODE_DIGITS; ++i)
        range *= 10;
    const uint32_t limit = UINT32_MAX - UINT32_MAX % range;
    uint32_t value;
    do {
        do {
            if (RAND_bytes((unsigned char *)&value, sizeof value) != 1)
                return false;
        } while (value >= limit);
        snprintf(code, MCP_PAIR_CODE_SIZE, "%0*u", MCP_PAIR_CODE_DIGITS, (unsigned)(value % range));
    } while (CRYPTO_memcmp(code, previous, MCP_PAIR_CODE_DIGITS) == 0);
    return true;
}

static bool new_ticket(struct mcp_pairing *pair)
{
    uint64_t now = az_mcp_clock();
    if (!now || !pair->fingerprint[0])
        return false;
    char code[MCP_PAIR_CODE_SIZE] = {0};
    bool generated = generate_code(code, pair->code);
    clear_ticket(pair);
    if (generated) {
        memcpy(pair->code, code, sizeof code);
        pair->expires = now + MCP_PAIR_DURATION;
    }
    OPENSSL_cleanse(code, sizeof code);
    return generated;
}

void mcp_pair_start(struct mcp_pairing *pair, struct mcp_runtime *runtime, const struct mcp_config *config)
{
    if (!new_ticket(pair)) {
        clear_ticket(pair);
        mcp_pair_offer(runtime, MCP_PAIR_UNAVAILABLE, NULL, NULL, NULL);
        return;
    }
    char url[MCP_URL_SIZE];
    snprintf(url, sizeof url, "https://%s:%u" MCP_PAIR_PATH, config->bind, config->port);
    mcp_pair_offer(runtime, MCP_PAIR_READY, url, pair->code, pair->fingerprint);
}

void mcp_pair_poll(struct mcp_pairing *pair, struct mcp_runtime *runtime, const struct mcp_config *config)
{
    switch (mcp_pair_take_action(runtime)) {
    case MCP_PAIR_START:
        mcp_pair_start(pair, runtime, config);
        return;
    case MCP_PAIR_CANCEL:
        mcp_pair_cancel(pair, runtime);
        return;
    case MCP_PAIR_NONE:
        break;
    }
    if (pair->expires && !az_mcp_before(az_mcp_clock(), pair->expires)) {
        clear_ticket(pair);
        mcp_pair_offer(runtime, MCP_PAIR_EXPIRED, NULL, NULL, NULL);
    }
}

static char *bundle_json(const struct mcp_pairing *pair, const struct mcp_config *config)
{
    cmcp_json_t *bundle = cmcp_json_new_object();
    if (!bundle)
        return NULL;
    char url[MCP_URL_SIZE];
    snprintf(url, sizeof url, "https://%s:%u/mcp", config->bind, config->port);
    cmcp_json_t *token = cmcp_json_new_string(config->token);
    mcp_set(bundle, "version", cmcp_json_new_int(1));
    mcp_set(bundle, "transport", cmcp_json_new_string("streamable-http"));
    mcp_set(bundle, "url", cmcp_json_new_string(url));
    mcp_set(bundle, "bearerToken", token);
    mcp_set(bundle, "caCertificatePem", pair->certificate ? cmcp_json_new_string(pair->certificate) : cmcp_json_new_null());
    char *wire = cmcp_json_object_len(bundle) == 5 ? cmcp_json_emit(bundle) : NULL;
    const cmcp_json_t *stored = cmcp_json_object_get(bundle, "bearerToken");
    if (stored)
        OPENSSL_cleanse(stored->str.s, stored->str.len);
    cmcp_json_free(bundle);
    return wire;
}

static bool same_origin(const struct mcp_config *config, const char *origin)
{
    char expected[MCP_URL_SIZE];
    if (config->port == 443)
        snprintf(expected, sizeof expected, "https://%s", config->bind);
    else
        snprintf(expected, sizeof expected, "https://%s:%u", config->bind, config->port);
    return origin && !strcmp(origin, expected);
}

static bool code_matches(struct evbuffer *input, const char *code)
{
    static const char prefix[] = "code=";
    const size_t prefix_length = sizeof prefix - 1;
    unsigned char form[sizeof prefix - 1 + MCP_PAIR_CODE_DIGITS];
    bool matches = evbuffer_get_length(input) == sizeof form &&
        evbuffer_remove(input, form, sizeof form) == sizeof form &&
        !memcmp(form, prefix, prefix_length) &&
        CRYPTO_memcmp(form + prefix_length, code, MCP_PAIR_CODE_DIGITS) == 0;
    OPENSSL_cleanse(form, sizeof form);
    return matches;
}

static int check_form(struct mcp_pairing *pair, struct mcp_runtime *runtime,
                      const struct mcp_config *config, struct evhttp_request *request)
{
    if (!pair->expires)
        return 410;
    struct evkeyvalq *headers = evhttp_request_get_input_headers(request);
    if (!same_origin(config, evhttp_find_header(headers, "Origin")))
        return 403;
    const char *type = evhttp_find_header(headers, "Content-Type");
    if (!type || strcmp(type, "application/x-www-form-urlencoded"))
        return 415;
    if (code_matches(evhttp_request_get_input_buffer(request), pair->code))
        return 200;
    if (++pair->attempts < MCP_PAIR_ATTEMPTS)
        return 403;
    clear_ticket(pair);
    mcp_pair_offer(runtime, MCP_PAIR_LOCKED, NULL, NULL, NULL);
    return 429;
}

static void pairing_headers(struct evkeyvalq *headers, const char *type)
{
    evhttp_add_header(headers, "Content-Type", type);
    evhttp_add_header(headers, "Cache-Control", "no-store");
    evhttp_add_header(headers, "Connection", "close");
    evhttp_add_header(headers, "X-Content-Type-Options", "nosniff");
    evhttp_add_header(headers, "Referrer-Policy", "same-origin");
    evhttp_add_header(headers, "Content-Security-Policy",
                      "default-src 'none'; form-action 'self'; frame-ancestors 'none'; "
                      "base-uri 'none'; style-src 'unsafe-inline'");
}

static void send_pair_page(struct evhttp_request *request, int status, const char *message)
{
    static const char page[] =
        "<!doctype html><html lang=en><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width, initial-scale=1\">"
        "<title>Deck MCP Setup</title><style>"
        ":root{color-scheme:light dark}body{font:1rem/1.55 system-ui,sans-serif;"
        "max-width:31rem;margin:10vh auto;padding:0 1.25rem}"
        "label,input{display:block}input,button{font:inherit;padding:.5rem;margin:.4rem 0}"
        "</style><main><h1>Deck MCP Setup</h1>%s"
        "<p>On the deck, open Deck MCP Settings and select Pair with Codex.</p>"
        "<form action=" MCP_PAIR_PATH " method=post autocomplete=off>"
        "<label for=code>%d-digit setup code</label>"
        "<input id=code name=code type=password inputmode=numeric pattern=\"[0-9]{%d}\" "
        "maxlength=%d required><button type=submit>Download connection file</button></form>"
        "<p>Ask your local coding assistant to configure Deck MCP using the downloaded "
        "<code>deck-mcp-connection.json</code>. The file contains a private credential.</p></main></html>";
    struct evbuffer *body = evbuffer_new();
    struct evkeyvalq *headers = evhttp_request_get_output_headers(request);
    pairing_headers(headers, "text/html; charset=utf-8");
    if (status == 405)
        evhttp_add_header(headers, "Allow", "GET, POST");
    if (!body) {
        evhttp_send_reply(request, 500, NULL, NULL);
        return;
    }
    if (evbuffer_add_printf(body, page, message, MCP_PAIR_CODE_DIGITS,
                            MCP_PAIR_CODE_DIGITS, MCP_PAIR_CODE_DIGITS) < 0)
        status = 500;
    evhttp_send_reply(request, status, NULL, body);
    evbuffer_free(body);
}

static bool send_bundle(struct evhttp_request *request, char *wire)
{
    if (!wire)
        return false;
    struct evbuffer *body = evbuffer_new();
    size_t length = strlen(wire);
    bool ready = body && evbuffer_add(body, wire, length) == 0;
    OPENSSL_cleanse(wire, length);
    free(wire);
    if (ready) {
        struct evkeyvalq *headers = evhttp_request_get_output_headers(request);
        pairing_headers(headers, "application/json");
        evhttp_add_header(headers, "Content-Disposition", "attachment; filename=\"deck-mcp-connection.json\"");
        evhttp_send_reply(request, 200, NULL, body);
    }
    if (body)
        evbuffer_free(body);
    return ready;
}

static const char *form_error(int status)
{
    switch (status) {
    case 403: return "<p role=alert>Code rejected. Check the code on the deck and try again.</p>";
    case 410: return "<p role=alert>No active setup code. Generate a new code on the deck.</p>";
    case 429: return "<p role=alert>Too many attempts. Generate a new code on the deck.</p>";
    case 415: return "<p role=alert>Submit the code using this form.</p>";
    default: return "<p role=alert>Could not download the file. Try again.</p>";
    }
}

void mcp_pair_http(struct mcp_pairing *pair, struct mcp_runtime *runtime,
                   const struct mcp_config *config, struct evhttp_request *request)
{
    switch (evhttp_request_get_command(request)) {
    case EVHTTP_REQ_GET:
        send_pair_page(request, 200, "");
        return;
    case EVHTTP_REQ_POST:
        break;
    default:
        send_pair_page(request, 405, "");
        return;
    }
    int status = check_form(pair, runtime, config, request);
    if (status == 200 && send_bundle(request, bundle_json(pair, config))) {
        clear_ticket(pair);
        mcp_pair_offer(runtime, MCP_PAIR_USED, NULL, NULL, NULL);
        return;
    }
    send_pair_page(request, status == 200 ? 500 : status, form_error(status));
}
