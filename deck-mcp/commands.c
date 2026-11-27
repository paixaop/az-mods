/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"
#include "azmod-data-helpers.h"
#include "azmod-timestamps.h"
#include "provider/wire.h"

#include <stdio.h>
#include <string.h>

#define NS_PER_SECOND UINT64_C(1000000000)
#define ARM_SECONDS 900

uint64_t mcp_receipt_deadline(uint64_t now)
{
    return now + MCP_RECEIPT_SECONDS * AZ_MCP_NS;
}

bool mcp_receipt_expired(uint64_t now, uint64_t deadline)
{
    return now && deadline && !az_mcp_before(now, deadline);
}

static az_command_result command_result(void)
{
    return (az_command_result){ .size = sizeof(az_command_result), .version = AZ_DATA_VERSION };
}

static void cancel_active(struct mcp_commands *commands)
{
    struct mcp_active_job *active = &commands->active;
    if (!active->waiting_terminal || active->cancel_sent)
        return;
    az_command_result ignored = command_result();
    int status = commands->api->command_cancel(MCP_ID, active->request_id, &ignored);
    active->cancel_sent = status != AZ_BUSY;
}

void mcp_commands_disarm(void *user)
{
    struct mcp_runtime *runtime = user;
    atomic_store(&runtime->commands.grant, 0);
    if (!(runtime->commands.active.result.intent.flags & AZ_COMMAND_PERSIST))
        cancel_active(&runtime->commands);
}

void mcp_commands_arm(struct mcp_runtime *runtime)
{
    struct mcp_commands *commands = &runtime->commands;
    uint64_t now = az_mcp_clock();
    if (!commands->api || !now || mcp_live_grant_generation(runtime, now) || commands->generation == UINT32_MAX)
        return;
    /* An expired grant must not inherit the new deadline between these stores. */
    mcp_commands_disarm(runtime);
    atomic_store(&commands->arm_deadline, now + ARM_SECONDS * NS_PER_SECOND);
    atomic_store(&commands->grant, ++commands->generation);
}

static bool has_commands(const az_commands_v1 *api)
{
    return AZ_COMMANDS_HAS(api, command_submit_async) &&
           AZ_COMMANDS_HAS(api, command_acknowledge) &&
           AZ_COMMANDS_HAS(api, command_cancel);
}

void mcp_commands_init(struct mcp_runtime *runtime)
{
    struct mcp_commands *commands = &runtime->commands;
    commands->api = NULL;
    commands->count = 0;
    commands->generation = 0;
    commands->replacement_open = false;
    commands->active = (struct mcp_active_job){0};
    atomic_store(&commands->grant, 0);
    atomic_store(&commands->arm_deadline, 0);
    const az_core_v1 *core = runtime->core;
    if (!AZ_HAS(core, command_service) ||
        !AZ_OBSERVATION_HAS(runtime->observation, command_target))
        return;
    const az_commands_v1 *api = core->command_service(AZ_COMMANDS_ABI);
    if (has_commands(api))
        commands->api = api;
}

void mcp_commands_reap(struct mcp_commands *commands, uint64_t now)
{
    size_t kept = 0;
    for (size_t i = 0; i < commands->count; ++i) {
        if (mcp_receipt_expired(now, commands->jobs[i].collect_until))
            continue;
        if (commands->active.present && commands->active.index == i)
            commands->active.index = kept;
        if (kept != i)
            commands->jobs[kept] = commands->jobs[i];
        ++kept;
    }
    commands->count = kept;
}

static struct mcp_job *find_job(struct mcp_commands *commands, const char *key, uint64_t now)
{
    mcp_commands_reap(commands, now);
    for (size_t i = 0; i < commands->count; ++i)
        if (!strcmp(commands->jobs[i].intent.key, key))
            return &commands->jobs[i];
    return NULL;
}

static bool same_parameters(const struct mcp_intent *a, const struct mcp_intent *b)
{
    return memcmp(&a->amount, &b->amount, sizeof a->amount) == 0 &&
           memcmp(&a->position, &b->position, sizeof a->position) == 0 &&
           memcmp(&a->end, &b->end, sizeof a->end) == 0 &&
           !strcmp(a->comment, b->comment) && a->batch_mode == b->batch_mode &&
           a->batch_count == b->batch_count &&
           !memcmp(a->batch, b->batch, a->batch_count * sizeof a->batch[0]);
}

static bool same_intent(const struct mcp_intent *a, const struct mcp_intent *b)
{
    return a->deck == b->deck && a->operation == b->operation && a->slot == b->slot &&
           a->load == b->load && a->cue_revision == b->cue_revision && a->flags == b->flags &&
           a->classification == b->classification && a->rgb == b->rgb && same_parameters(a, b);
}

static unsigned command_grant_at(const struct mcp_runtime *runtime, const struct mcp_intent *intent,
                                 uint64_t now)
{
    if (!(intent->flags & AZ_COMMAND_PERSIST))
        return mcp_live_grant_generation(runtime, now);
    return mcp_saved_grant_generation(runtime, now);
}

unsigned mcp_command_grant(const struct mcp_runtime *runtime, const struct mcp_intent *intent)
{
    return command_grant_at(runtime, intent, az_mcp_clock());
}

unsigned mcp_live_grant_generation(const struct mcp_runtime *runtime, uint64_t now)
{
    const struct mcp_commands *commands = &runtime->commands;
    unsigned generation = atomic_load(&commands->grant);
    if (!az_mcp_before(now, atomic_load(&commands->arm_deadline)))
        return 0;
    return generation == atomic_load(&commands->grant) ? generation : 0;
}

unsigned mcp_saved_grant_generation(const struct mcp_runtime *runtime, uint64_t now)
{
    unsigned generation = atomic_load(&runtime->saved_generation);
    if (!az_mcp_before(now, atomic_load(&runtime->saved_edits)))
        return 0;
    return generation == atomic_load(&runtime->saved_generation) ? generation : 0;
}

static uint64_t queue_lifetime(const struct mcp_intent *intent)
{
    return (intent->batch_mode == AZ_CUE_BATCH_REPLACE ? 30 : 2) * NS_PER_SECOND;
}

static int queue_locked(struct mcp_runtime *runtime, const struct mcp_intent *intent,
                        unsigned grant, struct mcp_job *out)
{
    struct mcp_commands *commands = &runtime->commands;
    const uint64_t arrival = az_mcp_clock();
    struct mcp_job *job = find_job(commands, intent->key, arrival);
    if (job) {
        if (!same_intent(&job->intent, intent) || grant != job->grant)
            return AZ_INVALID;
        *out = *job;
        return AZ_OK;
    }
    if (!grant || grant != command_grant_at(runtime, intent, az_mcp_clock()))
        return AZ_CANCELLED;
    if (commands->count == MCP_JOBS)
        return AZ_LIMIT;
    if (!arrival)
        return AZ_FAILED;
    job = &commands->jobs[commands->count++];
    *job = (struct mcp_job){
        .intent = *intent, .grant = grant, .deadline_ns = arrival + queue_lifetime(intent)
    };
    *out = *job;
    return AZ_OK;
}

int mcp_queue(struct mcp_runtime *runtime, const struct mcp_intent *intent, unsigned grant,
              struct mcp_job *out)
{
    pthread_mutex_lock(&runtime->lock);
    int status = queue_locked(runtime, intent, grant, out);
    pthread_mutex_unlock(&runtime->lock);
    return status;
}

bool mcp_job_status(struct mcp_runtime *runtime, const char *key, struct mcp_job *out)
{
    pthread_mutex_lock(&runtime->lock);
    struct mcp_job *job = find_job(&runtime->commands, key, az_mcp_clock());
    if (job)
        *out = *job;
    pthread_mutex_unlock(&runtime->lock);
    return job != NULL;
}

bool mcp_cancel_job(struct mcp_runtime *runtime, const char *key, struct mcp_job *out)
{
    pthread_mutex_lock(&runtime->lock);
    struct mcp_job *job = find_job(&runtime->commands, key, az_mcp_clock());
    if (job) {
        if (job->state <= JOB_ACCEPTED)
            job->cancel_requested = true;
        *out = *job;
    }
    pthread_mutex_unlock(&runtime->lock);
    return job != NULL;
}

static void settle(struct mcp_active_job *active, unsigned state, int reason)
{
    active->awaiting_confirmation = false;
    active->result.state = state;
    active->result.reason = reason;
    active->result.settled = true;
    active->changed = true;
}

static bool completion_shape(const az_command_completion *completion, uint64_t id)
{
    return completion && completion->size >= sizeof *completion &&
           completion->version == AZ_DATA_VERSION && completion->settled <= 1 &&
           az_command_result_matches(&completion->result, id);
}

static bool valid_terminal(const az_command_completion *completion)
{
    const az_command_result *result = &completion->result;
    if (result->outcome == AZ_COMMAND_APPLIED)
        return completion->settled && result->reason == AZ_OK && result->revision && result->observed_ns;
    if (result->outcome == AZ_COMMAND_UNCERTAIN)
        return true;
    return completion->settled &&
           (result->outcome == AZ_COMMAND_CANCELLED || result->outcome == AZ_COMMAND_REJECTED);
}

static bool matches_terminal(const az_command_result *a, const az_command_result *b)
{
    int clock = compare_timestamps(a->observed_ns, b->observed_ns, NS_PER_SECOND, NS_PER_SECOND, 1);
    if (clock == AZ_TIME_INVALID)
        return false;
    return clock == AZ_TIME_EQUAL && a->outcome == b->outcome &&
           a->reason == b->reason && a->revision == b->revision;
}

static void copy_terminal(struct mcp_active_job *active, const az_command_completion *completion)
{
    static const unsigned states[] = { JOB_ACCEPTED, JOB_APPLIED, JOB_REJECTED, JOB_CANCELLED, JOB_UNCERTAIN };
    active->terminal = completion->result;
    active->result.state = states[completion->result.outcome];
    active->result.reason = completion->result.reason;
    active->result.revision = completion->result.revision;
    active->result.observed_ns = completion->result.observed_ns;
    active->result.settled = completion->settled;
    active->waiting_terminal = false;
    active->waiting_release = true;
    active->changed = true;
}

static void receive_terminal(struct mcp_runtime *runtime, const az_command_completion *completion)
{
    struct mcp_active_job *active = &runtime->commands.active;
    if (!active->waiting_terminal || !valid_terminal(completion))
        return;
    copy_terminal(active, completion);
    int status = runtime->commands.api->command_acknowledge(MCP_ID, active->request_id);
    if (status != AZ_OK)
        runtime->core->log(MCP_ID, "Command acknowledgement refused (%d)", status);
}

static void receive_release(struct mcp_active_job *active, const az_command_completion *completion)
{
    if (!active->waiting_release || !completion->settled ||
        !matches_terminal(&completion->result, &active->terminal))
        return;
    active->waiting_release = false;
    active->result.settled = true;
    active->changed = true;
}

static void command_complete(const az_command_completion *completion, void *user)
{
    struct mcp_runtime *runtime = user;
    struct mcp_active_job *active = &runtime->commands.active;
    if (!active->present || !completion_shape(completion, active->request_id))
        return;
    if (completion->kind == AZ_COMMAND_COMPLETION_TERMINAL)
        receive_terminal(runtime, completion);
    else if (completion->kind == AZ_COMMAND_COMPLETION_RELEASED)
        receive_release(active, completion);
}

static void publish_active(struct mcp_commands *commands, uint64_t now)
{
    struct mcp_active_job *active = &commands->active;
    if (!active->changed)
        return;
    if (!active->waiting_terminal && !active->waiting_release) {
        active->result.collect_until = mcp_receipt_deadline(now);
        active->present = false;
    }
    active->result.cancel_requested = commands->jobs[active->index].cancel_requested;
    commands->jobs[active->index] = active->result;
    active->changed = false;
}

static void cancel_queued(struct mcp_runtime *runtime, uint64_t now)
{
    struct mcp_commands *commands = &runtime->commands;
    for (size_t i = 0; i < commands->count; ++i) {
        struct mcp_job *job = &commands->jobs[i];
        if (commands->active.present && commands->active.index == i)
            continue;
        if (job->state == JOB_QUEUED && (job->grant != command_grant_at(runtime, &job->intent, now) || job->cancel_requested)) {
            job->state = JOB_CANCELLED;
            job->reason = AZ_CANCELLED;
            job->settled = true;
            job->collect_until = mcp_receipt_deadline(now);
        }
    }
}

static void take_next(struct mcp_commands *commands)
{
    for (size_t i = 0; i < commands->count; ++i) {
        const struct mcp_job *job = &commands->jobs[i];
        if (job->state == JOB_QUEUED) {
            commands->active = (struct mcp_active_job){ .index = i, .result = *job, .present = true };
            return;
        }
    }
}

static bool target_matches(const az_command_target_v1 *target, const struct mcp_intent *intent)
{
    if (target->size < sizeof *target || target->version != AZ_DATA_VERSION ||
        target->cues.native_load_generation != intent->load)
        return false;
    return !intent->cue_revision || intent->cue_revision == target->cues.revision;
}

static int native_target(struct mcp_runtime *runtime, const struct mcp_intent *intent)
{
    az_command_target_v1 target = { .size = sizeof target };
    int status = runtime->observation->command_target((int)intent->deck, intent->load, &target);
    if (status != AZ_OK)
        return status;
    if (!target_matches(&target, intent))
        return AZ_STALE;
    if (intent->batch_mode)
        return AZ_COMMANDS_HAS(runtime->commands.api, cue_batch_submit_async) ? AZ_OK : AZ_UNSUPPORTED;
    return target.commands & (UINT64_C(1) << intent->operation) ? AZ_OK : AZ_UNSUPPORTED;
}

static void set_loop_amount(az_command_request *request, double amount)
{
    switch (request->operation) {
    case AZ_CMD_LOOP_LENGTH: request->beats = amount; break;
    case AZ_CMD_LOOP_MOVE:
    case AZ_CMD_LOOP_ADJUST_IN: request->position_s = amount; break;
    case AZ_CMD_LOOP_ADJUST_OUT: request->end_s = amount; break;
    }
}

static int submit_request(struct mcp_runtime *runtime, const struct mcp_intent *intent,
                          uint64_t deadline, uint64_t id)
{
    az_command_request request = {
        .size = sizeof request, .version = AZ_DATA_VERSION, .operation = intent->operation,
        .deck = intent->deck, .request_id = id, .expected_load_generation = intent->load,
        .deadline_ns = deadline, .flags = AZ_COMMAND_NATIVE_LOAD | intent->flags,
        .quantization = AZ_QUANTIZE_NATIVE, .entry_id = intent->slot,
        .expected_revision = (intent->flags & AZ_COMMAND_NATIVE_CUE_IDS) ? intent->cue_revision : 0,
        .position_s = intent->position, .end_s = intent->end, .classification = intent->classification,
        .rgb = intent->rgb
    };
    set_loop_amount(&request, intent->amount);
    memcpy(request.comment, intent->comment, sizeof request.comment);
    az_command_result result = command_result();
    return runtime->commands.api->command_submit_async(MCP_ID, &request, command_complete,
                                                        runtime, &result);
}

static int submit_batch(struct mcp_runtime *runtime, const struct mcp_intent *intent,
                        uint64_t now, uint64_t id)
{
    az_cue_batch_request request = { .size = sizeof request, .version = AZ_DATA_VERSION,
        .deck = intent->deck, .mode = intent->batch_mode, .request_id = id,
        .expected_load_generation = intent->load, .expected_revision = intent->cue_revision,
        .deadline_ns = now + 2 * NS_PER_SECOND, .count = intent->batch_count };
    memcpy(request.entries, intent->batch, intent->batch_count * sizeof intent->batch[0]);
    az_command_result result = command_result();
    return runtime->commands.api->cue_batch_submit_async(MCP_ID, &request, command_complete, runtime, &result);
}

static void confirm_replacement(int accepted, void *user)
{
    struct mcp_runtime *runtime = user;
    runtime->commands.replacement_open = false;
    struct mcp_active_job *active = &runtime->commands.active;
    if (!active->awaiting_confirmation)
        return;
    active->awaiting_confirmation = false;
    active->confirmed = accepted != 0;
    if (!accepted)
        settle(active, JOB_CANCELLED, AZ_CANCELLED);
}

static bool wait_confirmation(struct mcp_runtime *runtime, struct mcp_active_job *active)
{
    if (active->result.intent.batch_mode != AZ_CUE_BATCH_REPLACE || active->confirmed)
        return false;
    if (active->awaiting_confirmation)
        return true;
    /* Only the current owner can set awaiting_confirmation. A retired job's
     * modal remains occupied until the core delivers its one answer. */
    if (runtime->commands.replacement_open) {
        settle(active, JOB_REJECTED, AZ_BUSY);
        return true;
    }
    char message[112];
    snprintf(message, sizeof message, "Deck %u. Replace the saved cue bank with %u requested cues?",
             active->result.intent.deck + 1, active->result.intent.batch_count);
    const az_core_v1 *core = runtime->core;
    if (AZ_HAS(core, confirm) && core->confirm(MCP_ID, "Replace saved cues?", message,
                                             "Replace", confirm_replacement, runtime) == 0) {
        runtime->commands.replacement_open = true;
        active->awaiting_confirmation = true;
    } else
        settle(active, JOB_REJECTED, AZ_BUSY);
    return true;
}

static bool intent_revoked(struct mcp_runtime *runtime, const struct mcp_active_job *active,
                           uint64_t now)
{
    return active->result.cancel_requested ||
           active->result.grant != command_grant_at(runtime, &active->result.intent, now);
}

static bool active_lease(struct mcp_runtime *runtime, struct mcp_active_job *active, uint64_t now)
{
    if (intent_revoked(runtime, active, now)) {
        settle(active, JOB_CANCELLED, AZ_CANCELLED);
        return false;
    }
    if (!az_mcp_before(now, active->result.deadline_ns)) {
        settle(active, JOB_REJECTED, AZ_STALE);
        return false;
    }
    return true;
}

static void submit_active(struct mcp_runtime *runtime, uint64_t now)
{
    struct mcp_commands *commands = &runtime->commands;
    struct mcp_active_job *active = &commands->active;
    const struct mcp_intent *intent = &active->result.intent;
    if (!active_lease(runtime, active, now) || wait_confirmation(runtime, active))
        return;
    int status = native_target(runtime, intent);
    if (status != AZ_OK || commands->next_request == UINT64_MAX) {
        settle(active, JOB_REJECTED, status == AZ_OK ? AZ_FAILED : status);
        return;
    }
    now = az_mcp_clock();
    if (!active_lease(runtime, active, now))
        return;
    active->request_id = ++commands->next_request;
    status = intent->batch_mode ? submit_batch(runtime, intent, now, active->request_id)
                                : submit_request(runtime, intent, active->result.deadline_ns, active->request_id);
    if (status != AZ_OK) {
        settle(active, JOB_REJECTED, status);
        return;
    }
    active->result.state = JOB_ACCEPTED;
    active->waiting_terminal = true;
    active->changed = true;
}

static void expire_grant(struct mcp_runtime *runtime, uint64_t now)
{
    struct mcp_commands *commands = &runtime->commands;
    if (atomic_load(&commands->grant) && !mcp_live_grant_generation(runtime, now))
        mcp_commands_disarm(runtime);
}

void mcp_commands_tick(struct mcp_runtime *runtime, uint64_t now)
{
    struct mcp_commands *commands = &runtime->commands;
    if (!commands->api)
        return;
    expire_grant(runtime, now);
    struct mcp_active_job *active = &commands->active;

    if (pthread_mutex_trylock(&runtime->lock))
        return;
    publish_active(commands, now);
    if (active->present)
        active->result.cancel_requested = commands->jobs[active->index].cancel_requested;
    cancel_queued(runtime, now);
    if (!active->present)
        take_next(commands);
    pthread_mutex_unlock(&runtime->lock);
    if (active->waiting_terminal && intent_revoked(runtime, active, now))
        cancel_active(commands);
    if (active->present && active->result.state == JOB_QUEUED)
        submit_active(runtime, now);
}
