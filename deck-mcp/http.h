/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DECK_MCP_HTTP_H
#define DECK_MCP_HTTP_H

#include "deck-mcp.h"
#include "provider/wire.h"

#include <event2/http.h>
#include <openssl/ssl.h>

#define MCP_PROVIDER_LIMIT 4
#define MCP_PATH_SIZE 512
#define MCP_SETUP_CA "deck-mcp-ca.crt"
#define MCP_PAIR_PATH "/"
#define MCP_PAIR_DURATION (120 * AZ_MCP_NS)
#define MCP_PAIR_ATTEMPTS 5
struct mcp_pairing {
    char code[MCP_PAIR_CODE_SIZE], fingerprint[MCP_FINGERPRINT_SIZE];
    char *certificate;
    uint64_t expires;
    unsigned attempts;
};
struct mcp_provider_policy {
    char id[AZ_MCP_NAME_LIMIT + 1];
    enum az_mcp_grant grant;
};
struct mcp_config {
    char bind[16], origin[256], token[MCP_SECRET_SIZE];
    unsigned short port;
    struct mcp_provider_policy providers[MCP_PROVIDER_LIMIT];
    size_t provider_count;
};
bool mcp_load_config(const char *directory, struct mcp_config *config);
bool mcp_prepare_config(const char *directory, struct mcp_config *config);
bool mcp_lan_address(char address[16]);
bool mcp_file_missing(const char *directory, const char *name);
bool mcp_save_file(const char *directory, const char *name, const char *text, size_t length, bool replace);
bool mcp_generate_secret(char out[MCP_SECRET_SIZE]);
bool mcp_replace_token(const char *directory, const char token[MCP_SECRET_SIZE]);
SSL_CTX *mcp_tls_context(void);
BIO *mcp_open_pem(const char *directory, const char *name);
EVP_PKEY *mcp_read_key(BIO *bio);
SSL_CTX *mcp_load_tls(const char *directory);
SSL_CTX *mcp_setup_tls(const char *directory, const char *address);
void mcp_pair_init(struct mcp_pairing *pair, SSL_CTX *tls, const char *directory, bool generated);
void mcp_pair_close(struct mcp_pairing *pair);
void mcp_pair_cancel(struct mcp_pairing *pair, struct mcp_runtime *runtime);
void mcp_pair_poll(struct mcp_pairing *pair, struct mcp_runtime *runtime,
                   const struct mcp_config *config);
void mcp_pair_start(struct mcp_pairing *pair, struct mcp_runtime *runtime,
                    const struct mcp_config *config);
void mcp_pair_http(struct mcp_pairing *pair, struct mcp_runtime *runtime,
                   const struct mcp_config *config, struct evhttp_request *request);
int mcp_http_exchange(cmcp_server_t *server, const struct evkeyvalq *headers,
                      const char *body, size_t length, char **wire);

#endif
