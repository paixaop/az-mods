/* SPDX-License-Identifier: GPL-3.0-only */
#include "http.h"
#include "tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_BYTES 2048
#define PEM_BYTES_LIMIT (128 * 1024)

static bool file_path(char path[MCP_PATH_SIZE], const char *directory, const char *name)
{
    int length = snprintf(path, MCP_PATH_SIZE, "%s/%s", directory, name);
    return length >= 0 && length < MCP_PATH_SIZE;
}

bool mcp_file_missing(const char *directory, const char *name)
{
    char path[MCP_PATH_SIZE];
    struct stat info;
    return file_path(path, directory, name) && lstat(path, &info) != 0 && errno == ENOENT;
}

static int regular_file(const char *directory, const char *name, off_t limit)
{
    char path[MCP_PATH_SIZE];
    if (!file_path(path, directory, name))
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= limit)
        return fd;
    close(fd);
    return -1;
}

static cmcp_json_t *read_config(const char *directory)
{
    char buffer[CONFIG_BYTES + 1];
    int fd = regular_file(directory, "server.json", CONFIG_BYTES);
    if (fd < 0)
        return NULL;
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        return NULL;
    }
    size_t length = fread(buffer, 1, sizeof buffer, file);
    cmcp_json_t *json = NULL;
    if (!ferror(file) && length <= CONFIG_BYTES)
        json = cmcp_json_parse(buffer, length);
    mbedtls_platform_zeroize(buffer, sizeof buffer);
    fclose(file);
    return json;
}

static bool copy_setting(const cmcp_json_t *object, const char *key, char *out, size_t max)
{
    const cmcp_json_t *value = cmcp_json_object_get(object, key);
    if (!value)
        return true;
    const char *str = cmcp_json_string(value);
    size_t length = cmcp_json_string_len(value);
    if (!str || length >= max || memchr(str, '\0', length))
        return false;
    memcpy(out, str, length + 1);
    return true;
}

static bool load_port(const cmcp_json_t *json, struct mcp_config *config)
{
    const cmcp_json_t *port = cmcp_json_object_get(json, "port");
    if (!port)
        return true;
    if (port->type != CMCP_JSON_INT)
        return false;
    long long value = cmcp_json_int(port);
    if (value < 1024 || value > 65535)
        return false;
    config->port = (unsigned short)value;
    return true;
}

static bool valid_token(const char *str)
{
    size_t length = 2 * MCP_TOKEN_BYTES;
    return strlen(str) == length && strspn(str, "0123456789abcdefABCDEF") == length;
}

static bool provider_policy(struct mcp_provider_policy *policy, const char *id, const char *scope)
{
    if (!az_mcp_name(id) || !scope)
        return false;
    if (!strcmp(scope, "read"))
        policy->grant = AZ_MCP_READ;
    else if (!strcmp(scope, "live"))
        policy->grant = AZ_MCP_LIVE;
    else if (!strcmp(scope, "saved"))
        policy->grant = AZ_MCP_SAVED;
    else
        return false;
    snprintf(policy->id, sizeof policy->id, "%s", id);
    return true;
}

static bool load_providers(const cmcp_json_t *json, struct mcp_config *config)
{
    const cmcp_json_t *providers = cmcp_json_object_get(json, "providers");
    if (!providers)
        return true;
    if (providers->type != CMCP_JSON_OBJECT || providers->obj.len > MCP_PROVIDER_LIMIT)
        return false;
    for (size_t i = 0; i < providers->obj.len; ++i) {
        const char *id = providers->obj.keys[i];
        if (strlen(id) != providers->obj.key_lens[i] ||
            !provider_policy(&config->providers[i], id, az_mcp_string(providers, id)))
            return false;
    }
    config->provider_count = providers->obj.len;
    return true;
}

static bool load_settings(const cmcp_json_t *json, struct mcp_config *config)
{
    return copy_setting(json, "bind", config->bind, sizeof config->bind) &&
           copy_setting(json, "origin", config->origin, sizeof config->origin) &&
           copy_setting(json, "token", config->token, sizeof config->token) &&
           load_port(json, config) && load_providers(json, config);
}

static void cleanse_token(const cmcp_json_t *json)
{
    const cmcp_json_t *secret = cmcp_json_object_get(json, "token");
    if (secret && secret->type == CMCP_JSON_STRING)
        mbedtls_platform_zeroize(secret->str.s, secret->str.len);
}

static bool valid_bind(const char *bind)
{
    if (!strcmp(bind, "auto"))
        return true;
    struct in_addr address;
    return inet_pton(AF_INET, bind, &address) == 1 && address.s_addr != htonl(INADDR_ANY);
}

bool mcp_load_config(const char *directory, struct mcp_config *config)
{
    *config = (struct mcp_config){ .bind = "127.0.0.1", .port = 8765 };
    cmcp_json_t *json = read_config(directory);
    bool valid = json && load_settings(json, config) && valid_token(config->token) &&
                 valid_bind(config->bind);
    cleanse_token(json);
    cmcp_json_free(json);
    return valid;
}

bool mcp_generate_secret(char out[MCP_SECRET_SIZE])
{
    unsigned char bytes[MCP_TOKEN_BYTES];
    bool generated = psa_generate_random(bytes, sizeof bytes) == PSA_SUCCESS;
    if (generated) {
        for (size_t i = 0; i < sizeof bytes; ++i)
            snprintf(out + 2 * i, 3, "%02x", bytes[i]);
    }
    mbedtls_platform_zeroize(bytes, sizeof bytes);
    return generated;
}

static bool write_config(int fd, const char *text, size_t length)
{
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        return false;
    }
    bool written = fwrite(text, 1, length, file) == length &&
                   fflush(file) == 0 && fsync(fd) == 0;
    return fclose(file) == 0 && written;
}

static void sync_config_directory(const char *directory)
{
    int fd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

static bool publish_file(const char *temporary, const char *target, bool replace)
{
    struct stat info;
    if (!replace && (lstat(target, &info) == 0 || errno != ENOENT))
        return false;
    return rename(temporary, target) == 0;
}

bool mcp_save_file(const char *directory, const char *name, const char *text, size_t length, bool replace)
{
    char target[MCP_PATH_SIZE], temporary[MCP_PATH_SIZE];
    if (!file_path(target, directory, name) || !file_path(temporary, directory, ".mcp-write.XXXXXX"))
        return false;
    int fd = mkstemp(temporary);
    if (fd < 0)
        return false;
    if (!write_config(fd, text, length) || !publish_file(temporary, target, replace)) {
        unlink(temporary);
        return false;
    }
    sync_config_directory(directory);
    return true;
}

static char *config_with_token(cmcp_json_t *json, const char *token)
{
    cmcp_json_t *replacement = cmcp_json_new_string(token);
    if (!replacement)
        return NULL;
    cleanse_token(json);
    if (cmcp_json_object_set(json, "token", replacement) != CMCP_OK) {
        mbedtls_platform_zeroize(replacement->str.s, replacement->str.len);
        cmcp_json_free(replacement);
        return NULL;
    }
    return cmcp_json_emit(json);
}

bool mcp_replace_token(const char *directory, const char token[MCP_SECRET_SIZE])
{
    cmcp_json_t *json = read_config(directory);
    char *text = config_with_token(json, token);
    cleanse_token(json);
    cmcp_json_free(json);
    if (!text)
        return false;
    size_t length = strlen(text);
    bool saved = length <= CONFIG_BYTES && mcp_save_file(directory, "server.json", text, length, true);
    mbedtls_platform_zeroize(text, length);
    free(text);
    return saved;
}

static bool read_pem_data(FILE *file, struct mcp_pem *pem)
{
    pem->bytes = malloc(PEM_BYTES_LIMIT + 1);
    if (!pem->bytes)
        return false;
    pem->length = fread(pem->bytes, 1, PEM_BYTES_LIMIT + 1, file);
    if (ferror(file) || pem->length == 0 || pem->length > PEM_BYTES_LIMIT ||
        memchr(pem->bytes, '\0', pem->length)) {
        mcp_free_pem(pem);
        return false;
    }
    pem->bytes[pem->length] = '\0';
    return true;
}

bool mcp_read_pem(const char *directory, const char *name, struct mcp_pem *pem)
{
    *pem = (struct mcp_pem){0};
    int fd = regular_file(directory, name, PEM_BYTES_LIMIT);
    if (fd < 0)
        return false;
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        return false;
    }
    bool read = read_pem_data(file, pem);
    fclose(file);
    return read;
}

void mcp_free_pem(struct mcp_pem *pem)
{
    if (pem->bytes) {
        mbedtls_platform_zeroize(pem->bytes, pem->length);
        free(pem->bytes);
    }
    *pem = (struct mcp_pem){0};
}
