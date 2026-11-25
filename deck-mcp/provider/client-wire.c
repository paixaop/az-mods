/* SPDX-License-Identifier: GPL-3.0-only */
#include "client-wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIRE_NS (AZ_MCP_NS / 4)

static bool socket_options(int fd)
{
#ifdef __APPLE__
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes))
        return false;
#endif
    return fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static bool wait_socket(int fd, short events, uint64_t deadline)
{
    struct pollfd item = { .fd = fd, .events = events };
    while (az_mcp_before(az_mcp_clock(), deadline)) {
        int ready = poll(&item, 1, 25);
        if (ready > 0)
            return (item.revents & events) != 0;
        if (ready < 0 && errno != EINTR)
            return false;
    }
    return false;
}

static bool request_connection(int fd)
{
    struct sockaddr_un address;
    socklen_t length = az_mcp_address(&address);
    if (connect(fd, (const struct sockaddr *)&address, length) == 0)
        return true;
    if (errno != EINPROGRESS || !wait_socket(fd, POLLOUT, az_mcp_clock() + WIRE_NS))
        return false;
    int status;
    socklen_t size = sizeof status;
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &status, &size) == 0 && !status;
}

int az_mcp_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (socket_options(fd) && request_connection(fd) && az_mcp_peer(fd))
        return fd;
    close(fd);
    return -1;
}

static ssize_t transfer(int fd, void *buffer, size_t length, bool writing)
{
#ifdef MSG_NOSIGNAL
    if (writing)
        return send(fd, buffer, length, MSG_NOSIGNAL);
#else
    if (writing)
        return send(fd, buffer, length, 0);
#endif
    return recv(fd, buffer, length, 0);
}

static bool exact(int fd, void *buffer, size_t length, bool writing)
{
    uint64_t deadline = az_mcp_clock() + WIRE_NS;
    short events = writing ? POLLOUT : POLLIN;
    char *cursor = buffer;
    while (length && wait_socket(fd, events, deadline)) {
        ssize_t bytes = transfer(fd, cursor, length, writing);
        if (bytes > 0) {
            cursor += bytes;
            length -= (size_t)bytes;
        } else if (!bytes || (errno != EINTR && errno != EAGAIN)) {
            return false;
        }
    }
    return length == 0;
}

bool az_mcp_send(int fd, const cmcp_json_t *message)
{
    char *text = cmcp_json_emit(message);
    if (!text)
        return false;
    size_t length = strlen(text);
    uint32_t encoded = htonl((uint32_t)length);
    bool ok = length <= AZ_MCP_PROVIDER_FRAME &&
              exact(fd, &encoded, sizeof encoded, true) && exact(fd, text, length, true);
    free(text);
    return ok;
}

cmcp_json_t *az_mcp_receive(int fd)
{
    uint32_t encoded;
    if (!exact(fd, &encoded, sizeof encoded, false))
        return NULL;
    size_t length = ntohl(encoded);
    if (!length || length > AZ_MCP_PROVIDER_FRAME)
        return NULL;
    char *text = malloc(length);
    if (!text)
        return NULL;
    cmcp_json_t *result = exact(fd, text, length, false) ? cmcp_json_parse(text, length) : NULL;
    free(text);
    return result;
}
