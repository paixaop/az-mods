/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DECK_MCP_COMMANDS_H
#define DECK_MCP_COMMANDS_H

#include "azmod-mod.h"
#include <stdbool.h>
#include <stdatomic.h>

#define MCP_JOBS 32
#define MCP_KEY_SIZE 65

enum mcp_job_state { JOB_QUEUED, JOB_ACCEPTED, JOB_APPLIED, JOB_REJECTED,
                     JOB_CANCELLED, JOB_UNCERTAIN };
struct mcp_intent {
    uint32_t deck, operation, slot, classification, rgb, flags;
    uint64_t load, cue_revision;
    uint32_t batch_mode, batch_count;
    az_cue_batch_entry batch[AZ_CUE_BATCH_LIMIT];
    double amount, position, end;
    char comment[128];
    char key[MCP_KEY_SIZE];
};

struct mcp_job {
    struct mcp_intent intent;
    unsigned grant, state;
    bool settled, cancel_requested;
    uint64_t revision, observed_ns, deadline_ns, collect_until;
    int reason;
};

struct mcp_active_job {
    uint64_t request_id;
    size_t index;
    struct mcp_job result;
    az_command_result terminal;
    bool present, awaiting_confirmation, confirmed, waiting_terminal, waiting_release, cancel_sent, changed;
};

struct mcp_commands {
    const az_commands_v1 *api;
    atomic_uint grant;
    atomic_uint_fast64_t arm_deadline;
    unsigned generation;
    uint64_t next_request;
    size_t count;
    bool replacement_open;
    struct mcp_job jobs[MCP_JOBS];
    struct mcp_active_job active;
};

struct mcp_runtime;
void mcp_commands_init(struct mcp_runtime *runtime);
void mcp_commands_tick(struct mcp_runtime *runtime, uint64_t now);
void mcp_commands_disarm(void *user);
void mcp_commands_arm(struct mcp_runtime *runtime);
int mcp_queue(struct mcp_runtime *runtime, const struct mcp_intent *intent, unsigned grant,
              struct mcp_job *job);
unsigned mcp_command_grant(const struct mcp_runtime *runtime, const struct mcp_intent *intent);
bool mcp_cancel_job(struct mcp_runtime *runtime, const char *key, struct mcp_job *out);
bool mcp_job_status(struct mcp_runtime *runtime, const char *key, struct mcp_job *out);

#endif
