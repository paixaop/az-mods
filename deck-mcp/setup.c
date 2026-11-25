/* SPDX-License-Identifier: GPL-3.0-only */
#include "http.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

static bool create_config(const char *directory)
{
    char token[MCP_SECRET_SIZE];
    if (!mcp_generate_secret(token))
        return false;
    char json[160];
    int length = snprintf(json, sizeof json,
                          "{\"bind\":\"auto\",\"port\":8765,\"token\":\"%s\"}\n", token);
    bool saved = mcp_save_file(directory, "server.json", json, (size_t)length, false);
    OPENSSL_cleanse(token, sizeof token);
    OPENSSL_cleanse(json, sizeof json);
    return saved;
}

bool mcp_prepare_config(const char *directory, struct mcp_config *config)
{
    if (!mcp_file_missing(directory, "server.json"))
        return mcp_load_config(directory, config);
    return mcp_file_missing(directory, "server.crt") &&
           mcp_file_missing(directory, "server.key") &&
           create_config(directory) && mcp_load_config(directory, config);
}

static bool private_ip(uint32_t ip)
{
    return (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8;
}

static uint32_t lan_ip(const struct ifaddrs *interface)
{
    const struct sockaddr *address = interface->ifa_addr;
    unsigned flags = interface->ifa_flags;
    if (!address || address->sa_family != AF_INET ||
        (flags & (IFF_UP | IFF_RUNNING | IFF_LOOPBACK)) != (IFF_UP | IFF_RUNNING))
        return 0;
    uint32_t ip = ntohl(((const struct sockaddr_in *)address)->sin_addr.s_addr);
    return private_ip(ip) || (ip >> 16) == 0xa9fe ? ip : 0;
}

static uint32_t lan_rank(uint32_t ip)
{
    return (ip >> 16) == 0xa9fe ? ip | 0xf0000000u : ip;
}

static uint32_t pick_lan(const struct ifaddrs *interface)
{
    uint32_t best = 0;
    for (; interface; interface = interface->ifa_next) {
        uint32_t ip = lan_ip(interface);
        if (ip && (!best || lan_rank(ip) < lan_rank(best)))
            best = ip;
    }
    return best;
}

bool mcp_lan_address(char address[16])
{
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0)
        return false;
    uint32_t ip = pick_lan(interfaces);
    freeifaddrs(interfaces);
    struct in_addr result = { .s_addr = htonl(ip) };
    return ip && inet_ntop(AF_INET, &result, address, 16);
}
