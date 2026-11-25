/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_MCP_CLIENT_WIRE_H
#define AZ_MCP_CLIENT_WIRE_H

#include "wire.h"
int az_mcp_connect(void);
bool az_mcp_send(int fd, const cmcp_json_t *message);
cmcp_json_t *az_mcp_receive(int fd);

#endif
