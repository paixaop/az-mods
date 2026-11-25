/* SPDX-License-Identifier: GPL-3.0-only */
#include "wire.h"
#include "azmod-timestamps.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t az_mcp_clock(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) || time.tv_sec < 0)
        return 0;
    return (uint64_t)time.tv_sec * AZ_MCP_NS + (uint64_t)time.tv_nsec;
}

bool az_mcp_before(uint64_t now, uint64_t deadline)
{
    int result = compare_timestamps(now, deadline, AZ_MCP_NS, AZ_MCP_NS, 1);
    if (!now || !deadline || result == AZ_TIME_INVALID)
        return false;
    return result == AZ_TIME_BEFORE;
}

bool az_mcp_peer(int fd)
{
#ifdef __linux__
    struct ucred cred;
    socklen_t length = sizeof cred;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) == 0 &&
           length == sizeof cred && cred.pid == getpid() && cred.uid == geteuid();
#elif defined(__APPLE__)
    uid_t uid;
    gid_t gid;
    pid_t pid;
    socklen_t length = sizeof pid;
    return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid() &&
           getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &length) == 0 && pid == getpid();
#else
    (void)fd;
    return false;
#endif
}

socklen_t az_mcp_address(struct sockaddr_un *address)
{
    *address = (struct sockaddr_un){ .sun_family = AF_UNIX };
#ifdef __linux__
    int size = snprintf(address->sun_path + 1, sizeof address->sun_path - 1,
                        "az-deck-mcp.%ld", (long)getpid());
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + size);
#else
    int size = snprintf(address->sun_path, sizeof address->sun_path,
                        "/tmp/az-deck-mcp.%ld.sock", (long)getpid());
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + size);
#endif
}

bool az_mcp_name(const char *name)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_-";
    size_t length = name ? strlen(name) : 0;
    return length && length <= AZ_MCP_NAME_LIMIT && strspn(name, alphabet) == length;
}

const char *az_mcp_string(const cmcp_json_t *object, const char *key)
{
    const cmcp_json_t *node = cmcp_json_object_get(object, key);
    const char *value = cmcp_json_string(node);
    return value && strlen(value) == cmcp_json_string_len(node) ? value : NULL;
}

uint64_t az_mcp_number(const cmcp_json_t *object, const char *key)
{
    const cmcp_json_t *node = cmcp_json_object_get(object, key);
    if (!node || node->type != CMCP_JSON_INT || node->i <= 0)
        return 0;
    return (uint64_t)node->i;
}

int az_mcp_set(cmcp_json_t *object, const char *key, cmcp_json_t *value)
{
    int rc = cmcp_json_object_set(object, key, value);
    if (rc)
        cmcp_json_free(value);
    return rc;
}

cmcp_json_t *az_mcp_message(const char *type)
{
    cmcp_json_t *message = cmcp_json_new_object();
    if (az_mcp_set(message, "type", cmcp_json_new_string(type)) == 0)
        return message;
    cmcp_json_free(message);
    return NULL;
}
