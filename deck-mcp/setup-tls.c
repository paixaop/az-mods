/* SPDX-License-Identifier: GPL-3.0-only */
#include "http.h"
#include "tls.h"

#include <arpa/inet.h>
#include <mbedtls/oid.h>
#include <mbedtls/platform_util.h>
#include <string.h>
#include <time.h>

#define SETUP_IDENTITY "setup-ca.pem"
#define DAY_SECONDS 86400L
#define DATE_BYTES 15
#define GENERATED_PEM_LIMIT 4096

static const char authority_name[] = "CN=XDJ-AZ Deck MCP Local CA";
static const char server_name[] = "CN=XDJ-AZ Deck MCP";

static bool generate_key(struct mcp_identity *identity)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_ANY_HASH));
    psa_status_t status = psa_generate_key(&attributes, &identity->generated_key);
    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS && mbedtls_pk_wrap_psa(&identity->key, identity->generated_key) == 0;
}

static bool set_serial(mbedtls_x509write_cert *writer)
{
    unsigned char serial[16];
    if (psa_generate_random(serial, sizeof serial) != PSA_SUCCESS)
        return false;
    serial[0] = (serial[0] & 0x7f) | 1;
    return mbedtls_x509write_crt_set_serial_raw(writer, serial, sizeof serial) == 0;
}

static bool format_date(time_t timestamp, char result[DATE_BYTES])
{
    struct tm parts;
    return gmtime_r(&timestamp, &parts) && strftime(result, DATE_BYTES, "%Y%m%d%H%M%S", &parts) == DATE_BYTES - 1;
}

static bool set_validity(mbedtls_x509write_cert *writer, long days)
{
    time_t now = time(NULL);
    char before[DATE_BYTES], after[DATE_BYTES];
    return now != (time_t)-1 && format_date(now - DAY_SECONDS, before) &&
           format_date(now + days * DAY_SECONDS, after) &&
           mbedtls_x509write_crt_set_validity(writer, before, after) == 0;
}

static bool set_names(mbedtls_x509write_cert *writer, const char *subject, const char *issuer)
{
    return mbedtls_x509write_crt_set_subject_name(writer, subject) == 0 &&
           mbedtls_x509write_crt_set_issuer_name(writer, issuer) == 0;
}

static bool write_certificate(mbedtls_x509write_cert *writer, mbedtls_x509_crt *cert)
{
    unsigned char der[GENERATED_PEM_LIMIT];
    int length = mbedtls_x509write_crt_der(writer, der, sizeof der);
    return length > 0 && mbedtls_x509_crt_parse_der(cert, der + sizeof der - (size_t)length, (size_t)length) == 0;
}

static void set_keys(mbedtls_x509write_cert *writer, mbedtls_pk_context *subject, mbedtls_pk_context *issuer)
{
    mbedtls_x509write_crt_set_version(writer, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(writer, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(writer, subject);
    mbedtls_x509write_crt_set_issuer_key(writer, issuer);
}

static bool save_authority(const char *directory, const struct mcp_identity *authority)
{
    unsigned char pem[GENERATED_PEM_LIMIT];
    if (!mcp_certificate_pem(&authority->cert, pem, sizeof pem))
        return false;
    size_t length = strlen((const char *)pem);
    bool written = mbedtls_pk_write_key_pem(&authority->key, pem + length, sizeof pem - length) == 0 &&
                   mcp_save_file(directory, SETUP_IDENTITY, (const char *)pem, strlen((const char *)pem), false);
    mbedtls_platform_zeroize(pem, sizeof pem);
    return written;
}

static bool make_authority(struct mcp_identity *authority)
{
    if (!generate_key(authority))
        return false;
    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    set_keys(&writer, &authority->key, &authority->key);
    bool created = set_names(&writer, authority_name, authority_name) && set_serial(&writer) &&
                   set_validity(&writer, 3650) &&
                   mbedtls_x509write_crt_set_basic_constraints(&writer, 1, 0) == 0 &&
                   mbedtls_x509write_crt_set_key_usage(&writer, MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN) == 0 &&
                   write_certificate(&writer, &authority->cert);
    mbedtls_x509write_crt_free(&writer);
    return created;
}

static bool read_authority(const char *directory, struct mcp_identity *authority)
{
    struct mcp_pem pem;
    if (!mcp_read_pem(directory, SETUP_IDENTITY, &pem))
        return false;
    bool parsed = mbedtls_x509_crt_parse(&authority->cert, pem.bytes, pem.length + 1) == 0 &&
                  mbedtls_pk_parse_key(&authority->key, pem.bytes, pem.length + 1, NULL, 0) == 0;
    mcp_free_pem(&pem);
    return parsed && mbedtls_x509_crt_get_ca_istrue(&authority->cert) == 1 &&
           mcp_identity_matches(authority);
}

static bool load_authority(const char *directory, struct mcp_identity *authority)
{
    if (!mcp_file_missing(directory, SETUP_IDENTITY))
        return read_authority(directory, authority);
    return mcp_file_missing(directory, MCP_SETUP_CA) && make_authority(authority) &&
           save_authority(directory, authority);
}

static bool authority_matches(const char *directory, const mbedtls_x509_crt *cert)
{
    mbedtls_x509_crt existing;
    mbedtls_x509_crt_init(&existing);
    bool same = mcp_load_certificate(directory, MCP_SETUP_CA, &existing) &&
                existing.raw.len == cert->raw.len && memcmp(existing.raw.p, cert->raw.p, cert->raw.len) == 0;
    mbedtls_x509_crt_free(&existing);
    return same;
}

static bool export_authority(const char *directory, const mbedtls_x509_crt *cert)
{
    if (!mcp_file_missing(directory, MCP_SETUP_CA))
        return authority_matches(directory, cert);
    unsigned char pem[MCP_CERT_PEM_LIMIT];
    return mcp_certificate_pem(cert, pem, sizeof pem) &&
           mcp_save_file(directory, MCP_SETUP_CA, (const char *)pem, strlen((const char *)pem), false);
}

static bool server_usage(mbedtls_x509write_cert *writer, const char *address)
{
    struct in_addr ip;
    if (inet_pton(AF_INET, address, &ip) != 1)
        return false;
    mbedtls_x509_san_list san = { .node = {
        .type = MBEDTLS_X509_SAN_IP_ADDRESS,
        .san.unstructured_name = { .p = (unsigned char *)&ip, .len = sizeof ip },
    }};
    mbedtls_asn1_sequence usage = { .buf = {
        .tag = MBEDTLS_ASN1_OID, .p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH,
        .len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH),
    }};
    return mbedtls_x509write_crt_set_basic_constraints(writer, 0, 0) == 0 &&
           mbedtls_x509write_crt_set_key_usage(writer, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0 &&
           mbedtls_x509write_crt_set_ext_key_usage(writer, &usage) == 0 &&
           mbedtls_x509write_crt_set_subject_alternative_name(writer, &san) == 0;
}

static bool make_server(struct mcp_identity *server, struct mcp_identity *authority, const char *address)
{
    if (!generate_key(server))
        return false;
    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    set_keys(&writer, &server->key, &authority->key);
    bool created = set_names(&writer, server_name, authority_name) && set_serial(&writer) &&
                   set_validity(&writer, 365) && server_usage(&writer, address) &&
                   write_certificate(&writer, &server->cert);
    mbedtls_x509write_crt_free(&writer);
    return created;
}

struct mcp_tls *mcp_setup_tls(const char *directory, const char *address)
{
    struct mcp_identity authority = {0};
    struct mcp_tls *tls = mcp_tls_new();
    bool configured = tls && load_authority(directory, &authority) &&
                      export_authority(directory, &authority.cert) &&
                      make_server(&tls->identity, &authority, address) && mcp_tls_use_identity(tls);
    mcp_identity_free(&authority);
    if (configured)
        return tls;
    mcp_tls_free(tls);
    return NULL;
}
