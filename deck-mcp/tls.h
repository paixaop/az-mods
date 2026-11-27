/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DECK_MCP_TLS_H
#define DECK_MCP_TLS_H

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <stdbool.h>

#define MCP_CERT_PEM_LIMIT 16384

struct mcp_pem {
    unsigned char *bytes;
    size_t length;
};

struct mcp_identity {
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_svc_key_id_t generated_key;
};

struct mcp_tls {
    mbedtls_ssl_config config;
    struct mcp_identity identity;
};

bool mcp_read_pem(const char *directory, const char *name, struct mcp_pem *pem);
void mcp_free_pem(struct mcp_pem *pem);
bool mcp_load_certificate(const char *directory, const char *name, mbedtls_x509_crt *cert);
bool mcp_certificate_pem(const mbedtls_x509_crt *cert, unsigned char *out, size_t size);
bool mcp_certificate_signed_by(const mbedtls_x509_crt *cert, mbedtls_x509_crt *issuer);
bool mcp_identity_matches(const struct mcp_identity *identity);
void mcp_identity_free(struct mcp_identity *identity);
struct mcp_tls *mcp_tls_new(void);
bool mcp_tls_use_identity(struct mcp_tls *tls);
void mcp_tls_free(struct mcp_tls *tls);
struct mcp_tls *mcp_load_tls(const char *directory);
struct mcp_tls *mcp_setup_tls(const char *directory, const char *address);

#endif
