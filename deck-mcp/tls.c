/* SPDX-License-Identifier: GPL-3.0-only */
#include "tls.h"

#include <mbedtls/md.h>
#include <mbedtls/pem.h>
#include <mbedtls/platform_util.h>
#include <stdlib.h>
#include <string.h>

bool mcp_load_certificate(const char *directory, const char *name, mbedtls_x509_crt *cert)
{
    struct mcp_pem pem;
    if (!mcp_read_pem(directory, name, &pem))
        return false;
    bool loaded = mbedtls_x509_crt_parse(cert, pem.bytes, pem.length + 1) == 0;
    mcp_free_pem(&pem);
    return loaded;
}

bool mcp_certificate_pem(const mbedtls_x509_crt *cert, unsigned char *out, size_t size)
{
    size_t written;
    return mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n",
                                    cert->raw.p, cert->raw.len, out, size, &written) == 0;
}

bool mcp_certificate_signed_by(const mbedtls_x509_crt *cert, mbedtls_x509_crt *issuer)
{
    /* Verification against an identical trust anchor skips the self-signature.
     * Use the signature metadata from the pinned Mbed TLS parser instead. */
    mbedtls_md_type_t digest = cert->MBEDTLS_PRIVATE(sig_md);
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(digest);
    if (!md)
        return false;
    unsigned char hash[MBEDTLS_MD_MAX_SIZE];
    const mbedtls_x509_buf *signature = &cert->MBEDTLS_PRIVATE(sig);
    return mbedtls_md(md, cert->tbs.p, cert->tbs.len, hash) == 0 &&
           mbedtls_pk_verify_ext(cert->MBEDTLS_PRIVATE(sig_pk), &issuer->pk, digest,
                                 hash, mbedtls_md_get_size(md), signature->p, signature->len) == 0;
}

bool mcp_identity_matches(const struct mcp_identity *identity)
{
    /* Parsed RSA and wrapped PSA private keys can lack a cached public key.
     * Serializing both public keys also covers those contexts. */
    unsigned char cert_key[PSA_EXPORT_PUBLIC_KEY_MAX_SIZE + 64];
    unsigned char file_key[sizeof cert_key];
    int cert_length = mbedtls_pk_write_pubkey_der(&identity->cert.pk, cert_key, sizeof cert_key);
    int file_length = mbedtls_pk_write_pubkey_der(&identity->key, file_key, sizeof file_key);
    return cert_length > 0 && cert_length == file_length &&
           memcmp(cert_key + sizeof cert_key - cert_length, file_key + sizeof file_key - file_length,
                  (size_t)cert_length) == 0;
}

void mcp_identity_free(struct mcp_identity *identity)
{
    mbedtls_pk_free(&identity->key);
    mbedtls_x509_crt_free(&identity->cert);
    if (!mbedtls_svc_key_id_is_null(identity->generated_key))
        psa_destroy_key(identity->generated_key);
}

struct mcp_tls *mcp_tls_new(void)
{
    struct mcp_tls *tls = calloc(1, sizeof *tls);
    if (!tls)
        return NULL;
    mbedtls_ssl_config_init(&tls->config);
    mbedtls_x509_crt_init(&tls->identity.cert);
    mbedtls_pk_init(&tls->identity.key);
    if (mbedtls_ssl_config_defaults(&tls->config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        mcp_tls_free(tls);
        return NULL;
    }
    mbedtls_ssl_conf_min_tls_version(&tls->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_renegotiation(&tls->config, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
    mbedtls_ssl_conf_new_session_tickets(&tls->config, 0);
    return tls;
}

bool mcp_tls_use_identity(struct mcp_tls *tls)
{
    struct mcp_identity *identity = &tls->identity;
    return mcp_identity_matches(identity) &&
           mbedtls_ssl_conf_own_cert(&tls->config, &identity->cert, &identity->key) == 0;
}

void mcp_tls_free(struct mcp_tls *tls)
{
    if (!tls)
        return;
    mbedtls_ssl_config_free(&tls->config);
    mcp_identity_free(&tls->identity);
    free(tls);
}

static bool load_server_key(const char *directory, mbedtls_pk_context *key)
{
    struct mcp_pem pem;
    if (!mcp_read_pem(directory, "server.key", &pem))
        return false;
    bool loaded = mbedtls_pk_parse_key(key, pem.bytes, pem.length + 1, NULL, 0) == 0;
    mcp_free_pem(&pem);
    return loaded;
}

struct mcp_tls *mcp_load_tls(const char *directory)
{
    struct mcp_tls *tls = mcp_tls_new();
    if (tls && mcp_load_certificate(directory, "server.crt", &tls->identity.cert) &&
        load_server_key(directory, &tls->identity.key) && mcp_tls_use_identity(tls))
        return tls;
    mcp_tls_free(tls);
    return NULL;
}
