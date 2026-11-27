/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CUES_STORE_H
#define CUES_STORE_H
#include "cues.h"
/* Load configuration before registering UI, then start storage after startup. */
int cues_store_init(const az_core_v1 *, cues_config *, char *, size_t);
int cues_store_start(char *, size_t);
/* Requests and results are nonblocking UI handoffs. */
int cues_store_request(const cues_config *, unsigned revision);
int cues_store_take(unsigned *revision, char *, size_t);
#endif
