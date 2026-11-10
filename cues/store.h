/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CUES_STORE_H
#define CUES_STORE_H
#include "cues.h"
enum { CUES_STORE_SAVE = 1, CUES_STORE_IMPORT, CUES_STORE_EXPORT };
/* init may read files; later requests/take are nonblocking UI handoffs. */
int cues_store_init(const az_core_v1 *, cues_config *, char *, size_t);
int cues_store_request(int operation, const cues_config *, unsigned revision);
int cues_store_take(cues_config *, int *operation, unsigned *revision, char *, size_t);
#endif
