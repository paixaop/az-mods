/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod.h"
#include <stdlib.h>
#include <time.h>

bool sp_stopped(const SpRuntime *runtime)
{
    return atomic_load(&runtime->stop);
}

bool sp_cancelled(void *user)
{
    const SpRuntime *runtime = user;
    return sp_stopped(runtime) || atomic_load(&runtime->cancel);
}

void sp_delay(void)
{
    const struct timespec delay = {.tv_nsec = 20000000};
    nanosleep(&delay, NULL);
}

void sp_clear_order(SpOrder *order)
{
    free(order->rows);
    *order = (SpOrder){0};
}

void sp_discard_plan(SpModel *model)
{
    free(model->target);
    free(model->moves);
    free(model->timeline.starts);
    model->timeline = (SpTimeline){0};
    model->target = NULL;
    model->moves = NULL;
    model->move_count = 0;
    model->verified = 0;
    model->pending_move = (az_playlist_move_result_v1){0};
    model->can_resume = false;
}

void sp_discard_selection(SpModel *model)
{
    sp_discard_plan(model);
    sp_clear_order(&model->source);
    sp_clear_track_setup(&model->setup);
    free(model->tracks);
    free(model->inputs);
    model->tracks = NULL;
    model->inputs = NULL;
}

void sp_release_model(SpModel *model)
{
    sp_discard_selection(model);
}

static void score_track(SpRuntime *runtime, int index, bool refresh)
{
    SpModel *model = &runtime->model;
    SpInput *input = &model->inputs[index];
    if (!refresh && sp_cache_read(runtime, index)) {
        ++model->cache_hits;
    } else {
        sp_analyze_track(runtime, index);
        if (!sp_cancelled(runtime))
            sp_cache_write(runtime, index, refresh);
    }
    model->tracks[index].energy = input->score;
    bool known = input->energy != SP_MISSING;
    model->tracks[index].energy_known = known;
    model->unknown_energy += !known;
    model->fallback_energy += known && (input->energy != runtime->energy_choice.mode ||
        input->encoding != AZ_WAVE_PWV7_BANDS);
}

static int score_playlist(SpRuntime *runtime)
{
    SpModel *model = &runtime->model;
    int count = (int)model->source.identity.total;
    int status = sp_refresh_tracks(runtime);
    if (status != AZ_OK)
        return status;
    atomic_store(&runtime->total, count);
    model->cache_hits = 0;
    model->unknown_energy = 0;
    model->fallback_energy = 0;
    for (int i = 0; i < count; ++i) {
        if (sp_cancelled(runtime))
            return AZ_CANCELLED;
        score_track(runtime, i, runtime->job == SP_REFRESH);
        atomic_store(&runtime->progress, i + 1);
    }
    return sp_order_check(runtime);
}

static bool compute_order(SpRuntime *runtime, int count)
{
    SpModel *model = &runtime->model;
    SpOptions options = runtime->options;
    if (model->setup.sequence_count)
        options.links = model->setup.links;
    return sp_plan_order(model->tracks, count, &options, model->target);
}

static int plan_playlist(SpRuntime *runtime)
{
    SpModel *model = &runtime->model;
    int count = (int)model->source.identity.total;
    model->target = malloc((size_t)count * sizeof *model->target);
    model->moves = calloc((size_t)count, sizeof *model->moves);
    if (!model->target || !model->moves)
        return AZ_FAILED;
    if (!sp_normalize_energy(model->tracks, count))
        return AZ_FAILED;
    if (!compute_order(runtime, count))
        return AZ_FAILED;
    model->move_count = sp_plan_moves(model->target, count, model->moves, &runtime->options);
    if (model->move_count < 0)
        return AZ_FAILED;
    if (!sp_build_timeline(model, &runtime->options))
        return AZ_FAILED;
    model->can_resume = model->move_count > 0;
    return sp_order_check(runtime);
}

int sp_create_preview(SpRuntime *runtime)
{
    SpModel *model = &runtime->model;
    sp_discard_plan(model);
    runtime->sync_failed = false;
    int status = score_playlist(runtime);
    if (status == AZ_OK)
        status = plan_playlist(runtime);
    if (sp_cancelled(runtime))
        status = AZ_CANCELLED;
    if (status != AZ_OK)
        sp_discard_plan(model);
    return status;
}

static bool match_order(const SpOrder *origin, const SpOrder *current, const int *expected)
{
    if (!sp_same_source(origin, current))
        return false;
    for (uint32_t i = 0; i < origin->identity.total; ++i) {
        const az_playlist_entry_v1 *a = &origin->rows[expected[i]], *b = &current->rows[i];
        if (a->entry_id != b->entry_id || a->track_id != b->track_id)
            return false;
    }
    return true;
}

static int read_back(SpRuntime *runtime, const int *expected, uint64_t *revision)
{
    SpOrder current = {0};
    int status = AZ_BUSY;
    for (int i = 0; i < 100; ++i) {
        status = sp_read_order(runtime, &current);
        if (status != AZ_BUSY)
            break;
        sp_delay();
    }
    if (status != AZ_OK)
        return status;
    if (!match_order(&runtime->model.source, &current, expected))
        status = AZ_STALE;
    *revision = current.identity.revision;
    sp_clear_order(&current);
    return status;
}

static az_playlist_move_v1 make_request(const SpModel *model, SpMove move, uint64_t revision)
{
    return (az_playlist_move_v1){.size = sizeof(az_playlist_move_v1), .version = AZ_DATA_VERSION,
        .stick = (uint32_t)model->stick, .playlist_id = (uint32_t)model->playlist,
        .from = (uint32_t)move.from + 1, .to = (uint32_t)move.to + 1,
        .source_generation = model->source.identity.source_generation,
        .library_id = model->source.identity.library_id, .revision = revision,
        .entry_id = model->source.rows[move.entry].entry_id};
}

static int wait_for_move(SpRuntime *runtime, uint64_t receipt, az_playlist_move_result_v1 *result)
{
    for (int i = 0; i < 1500; ++i) {
        int status = runtime->library->playlist_move_status(SP_ID, receipt, result);
        if (status != AZ_PENDING && status != AZ_BUSY)
            return status;
        sp_delay();
    }
    return AZ_FAILED;
}

static int verify_move(SpRuntime *runtime, int *expected, uint64_t *revision)
{
    SpModel *model = &runtime->model;
    SpMove move = model->moves[model->verified];
    sp_relocate(expected, move.from, move.to);
    int status = read_back(runtime, expected, revision);
    if (status != AZ_OK)
        return status;
    az_playlist_move_result_v1 result = model->pending_move;
    model->pending_move = (az_playlist_move_result_v1){0};
    ++model->verified;
    atomic_store(&runtime->progress, model->verified);
    runtime->sync_failed = result.device_library != AZ_PDB_CURRENT;
    if (runtime->sync_failed)
        return AZ_FAILED;
    return result.outcome == AZ_PLAYLIST_MOVED ? result.status : AZ_FAILED;
}

static int apply_move(SpRuntime *runtime, int *expected, uint64_t *revision)
{
    SpModel *model = &runtime->model;
    SpMove move = model->moves[model->verified];
    az_playlist_move_v1 request = make_request(model, move, *revision);
    uint64_t receipt = 0;
    int status = runtime->library->playlist_move(SP_ID, &request, &receipt);
    if (status != AZ_PENDING)
        return status == AZ_OK ? AZ_FAILED : status;
    az_playlist_move_result_v1 result = {.size = sizeof result, .version = AZ_DATA_VERSION};
    status = wait_for_move(runtime, receipt, &result);
    if (status != AZ_OK) {
        model->can_resume = false;
        return status;
    }
    if (result.outcome == AZ_PLAYLIST_NOT_MOVED)
        return result.status == AZ_OK ? AZ_FAILED : result.status;
    model->pending_move = result;
    return verify_move(runtime, expected, revision);
}

static int read_resume_order(SpRuntime *runtime, int *expected, uint64_t *revision)
{
    const SpModel *model = &runtime->model;
    if (model->pending_move.outcome != AZ_PLAYLIST_NOT_MOVED)
        return verify_move(runtime, expected, revision);
    int status = read_back(runtime, expected, revision);
    if (status == AZ_OK && !model->verified && *revision != model->source.identity.revision)
        return AZ_STALE;
    return status;
}

static int apply_moves(SpRuntime *runtime, int *expected)
{
    SpModel *model = &runtime->model;
    uint64_t revision = 0;
    int status = read_resume_order(runtime, expected, &revision);
    while (status == AZ_OK && model->verified < model->move_count) {
        if (sp_cancelled(runtime))
            return AZ_CANCELLED;
        status = apply_move(runtime, expected, &revision);
    }
    return status;
}

int sp_apply_preview(SpRuntime *runtime)
{
    SpModel *model = &runtime->model;
    if (!model->can_resume)
        return AZ_INVALID;
    runtime->sync_failed = false;
    int count = (int)model->source.identity.total;
    int *expected = malloc((size_t)count * sizeof *expected);
    if (!expected)
        return AZ_FAILED;
    for (int i = 0; i < count; ++i)
        expected[i] = i;
    for (int i = 0; i < model->verified; ++i)
        sp_relocate(expected, model->moves[i].from, model->moves[i].to);
    atomic_store(&runtime->total, model->move_count);
    atomic_store(&runtime->progress, model->verified);
    int status = apply_moves(runtime, expected);
    model->can_resume = model->can_resume && model->verified < model->move_count && status != AZ_STALE;
    free(expected);
    return status;
}

bool sp_post_job(SpRuntime *runtime, enum SpJob job)
{
    if (atomic_load(&runtime->phase) != SP_IDLE)
        return false;
    runtime->job = job;
    atomic_store(&runtime->cancel, false);
    atomic_store(&runtime->progress, 0);
    atomic_store(&runtime->total, 0);
    atomic_store(&runtime->phase, SP_WORKING);
    return true;
}

static int execute_job(SpRuntime *runtime)
{
    switch (runtime->job) {
    case SP_LOAD: return sp_load_target(runtime);
    case SP_PREVIEW:
    case SP_REFRESH: return sp_create_preview(runtime);
    case SP_APPLY: return sp_apply_preview(runtime);
    default: return AZ_INVALID;
    }
}

void *sp_worker(void *user)
{
    SpRuntime *runtime = user;
    while (!sp_stopped(runtime)) {
        if (atomic_load(&runtime->phase) == SP_WORKING) {
            runtime->result = execute_job(runtime);
            atomic_store(&runtime->phase, SP_DONE);
        } else {
            const struct timespec idle = {.tv_nsec = 100000000};
            nanosleep(&idle, NULL);
        }
    }
    sp_release_model(&runtime->model);
    return NULL;
}
