/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_MCP_WIRE_H
#define AZ_MCP_WIRE_H

#include "az-mcp-provider.h"
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/un.h>

#define AZ_MCP_NS UINT64_C(1000000000)
#define AZ_MCP_LEASE_NS (2 * AZ_MCP_NS)
#define AZ_MCP_CALL_NS (2 * AZ_MCP_NS)
#define AZ_MCP_NAME_LIMIT 32

uint64_t az_mcp_clock(void);
bool az_mcp_before(uint64_t now, uint64_t deadline);
bool az_mcp_peer(int fd);
socklen_t az_mcp_address(struct sockaddr_un *address);
bool az_mcp_name(const char *name);
const char *az_mcp_string(const cmcp_json_t *object, const char *key);
uint64_t az_mcp_number(const cmcp_json_t *object, const char *key);
int az_mcp_set(cmcp_json_t *object, const char *key, cmcp_json_t *value);
cmcp_json_t *az_mcp_message(const char *type);

#endif
