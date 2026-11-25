/* SPDX-License-Identifier: GPL-3.0-only */
#include "http.h"

#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <stdio.h>

#define SETUP_IDENTITY "setup-ca.pem"
#define DAY_SECONDS 86400L

struct identity {
    X509 *cert;
    EVP_PKEY *key;
};

static void free_identity(struct identity *identity)
{
    X509_free(identity->cert);
    EVP_PKEY_free(identity->key);
}

static bool random_serial(X509 *cert)
{
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof bytes) != 1)
        return false;
    BIGNUM *serial = BN_bin2bn(bytes, sizeof bytes, NULL);
    bool ok = serial && BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(cert));
    BN_free(serial);
    return ok;
}

static bool subject_name(X509 *cert, const char *name)
{
    X509_NAME *subject = X509_get_subject_name(cert);
    return X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                      (const unsigned char *)name, -1, -1, 0) == 1;
}

static bool init_identity(struct identity *identity, const char *name, long days)
{
    identity->key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1");
    identity->cert = X509_new();
    X509 *cert = identity->cert;
    return cert && identity->key && X509_set_version(cert, 2) == 1 &&
           random_serial(cert) && subject_name(cert, name) &&
           X509_gmtime_adj(X509_getm_notBefore(cert), -DAY_SECONDS) &&
           X509_gmtime_adj(X509_getm_notAfter(cert), days * DAY_SECONDS) &&
           X509_set_pubkey(cert, identity->key) == 1;
}

static bool add_extension(X509 *cert, int field, const char *value)
{
    X509_EXTENSION *extension = X509V3_EXT_conf_nid(NULL, NULL, field, value);
    bool ok = extension && X509_add_ext(cert, extension, -1) == 1;
    X509_EXTENSION_free(extension);
    return ok;
}

static bool sign_certificate(struct identity *identity, const struct identity *issuer)
{
    X509_NAME *name = X509_get_subject_name(issuer->cert);
    return X509_set_issuer_name(identity->cert, name) == 1 &&
           X509_sign(identity->cert, issuer->key, EVP_sha256()) > 0;
}

static bool save_pem(const char *directory, const char *name, BIO *pem, bool secret)
{
    char *bytes = NULL;
    long length = BIO_get_mem_data(pem, &bytes);
    bool saved = length > 0 && mcp_save_file(directory, name, bytes, (size_t)length, false);
    if (secret && length > 0)
        OPENSSL_cleanse(bytes, (size_t)length);
    return saved;
}

static bool save_authority(const char *directory, const struct identity *authority)
{
    BIO *pem = BIO_new(BIO_s_mem());
    bool written = pem && PEM_write_bio_X509(pem, authority->cert) == 1 &&
        PEM_write_bio_PrivateKey(pem, authority->key, NULL, NULL, 0, NULL, NULL) == 1;
    bool saved = written && save_pem(directory, SETUP_IDENTITY, pem, true);
    BIO_free(pem);
    return saved;
}

static bool create_authority(const char *directory, struct identity *authority)
{
    if (!mcp_file_missing(directory, MCP_SETUP_CA) ||
        !init_identity(authority, "XDJ-AZ Deck MCP Local CA", 3650))
        return false;
    X509 *cert = authority->cert;
    return add_extension(cert, NID_basic_constraints, "critical,CA:TRUE,pathlen:0") &&
           add_extension(cert, NID_key_usage, "critical,keyCertSign,cRLSign") &&
           sign_certificate(authority, authority) && save_authority(directory, authority);
}

static bool load_authority(const char *directory, struct identity *authority)
{
    if (mcp_file_missing(directory, SETUP_IDENTITY))
        return create_authority(directory, authority);
    BIO *pem = mcp_open_pem(directory, SETUP_IDENTITY);
    if (!pem)
        return false;
    authority->cert = PEM_read_bio_X509(pem, NULL, NULL, NULL);
    authority->key = mcp_read_key(pem);
    BIO_free(pem);
    return authority->cert && authority->key &&
           X509_check_ca(authority->cert) > 0 &&
           X509_check_private_key(authority->cert, authority->key) == 1;
}

static bool export_authority(const char *directory, X509 *cert)
{
    if (!mcp_file_missing(directory, MCP_SETUP_CA)) {
        BIO *pem = mcp_open_pem(directory, MCP_SETUP_CA);
        X509 *existing = pem ? PEM_read_bio_X509(pem, NULL, NULL, NULL) : NULL;
        bool matches = existing && X509_cmp(existing, cert) == 0;
        X509_free(existing);
        BIO_free(pem);
        return matches;
    }
    BIO *pem = BIO_new(BIO_s_mem());
    bool exported = pem && PEM_write_bio_X509(pem, cert) == 1 &&
                    save_pem(directory, MCP_SETUP_CA, pem, false);
    BIO_free(pem);
    return exported;
}

static bool create_server(struct identity *server, const struct identity *authority, const char *address)
{
    char san[20];
    int length = snprintf(san, sizeof san, "IP:%s", address);
    if (length < 0 || (size_t)length >= sizeof san || !init_identity(server, "XDJ-AZ Deck MCP", 365))
        return false;
    X509 *cert = server->cert;
    return add_extension(cert, NID_basic_constraints, "critical,CA:FALSE") &&
           add_extension(cert, NID_key_usage, "critical,digitalSignature") &&
           add_extension(cert, NID_ext_key_usage, "serverAuth") &&
           add_extension(cert, NID_subject_alt_name, san) && sign_certificate(server, authority);
}

static SSL_CTX *server_context(const struct identity *server)
{
    SSL_CTX *ctx = mcp_tls_context();
    if (ctx && SSL_CTX_use_certificate(ctx, server->cert) == 1 &&
        SSL_CTX_use_PrivateKey(ctx, server->key) == 1 && SSL_CTX_check_private_key(ctx) == 1)
        return ctx;
    SSL_CTX_free(ctx);
    return NULL;
}

SSL_CTX *mcp_setup_tls(const char *directory, const char *address)
{
    struct identity authority = {0}, server = {0};
    SSL_CTX *ctx = NULL;
    if (load_authority(directory, &authority) && export_authority(directory, authority.cert) &&
        create_server(&server, &authority, address))
        ctx = server_context(&server);
    free_identity(&server);
    free_identity(&authority);
    return ctx;
}
