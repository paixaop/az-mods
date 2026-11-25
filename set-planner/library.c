/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool sp_same_source(const SpOrder *a, const SpOrder *b)
{
    return a->identity.source_generation == b->identity.source_generation &&
        a->identity.library_id == b->identity.library_id && a->identity.total == b->identity.total;
}

static int order_page(SpRuntime *runtime, uint32_t offset, az_playlist_entry_v1 *rows,
                      uint32_t capacity, az_playlist_order_v1 *identity)
{
    const SpModel *model = &runtime->model;
    *identity = (az_playlist_order_v1){.size = sizeof *identity, .version = AZ_DATA_VERSION};
    return runtime->library->playlist_order(SP_ID, model->stick, model->playlist, offset,
                                            rows, capacity, identity);
}

static bool page_valid(const SpOrder *order, const SpOrder *page, uint32_t offset, uint32_t capacity)
{
    return sp_same_source(order, page) && order->identity.revision == page->identity.revision &&
        page->identity.returned == capacity && offset + capacity <= order->identity.total;
}

static bool rows_valid(const az_playlist_entry_v1 *rows, uint32_t count, uint32_t offset)
{
    for (uint32_t i = 0; i < count; ++i)
        if (!rows[i].entry_id || !rows[i].track_id || rows[i].position != offset + i + 1)
            return false;
    return true;
}

static int remaining_pages(SpRuntime *runtime, SpOrder *order, uint32_t first)
{
    uint32_t total = order->identity.total;
    for (uint32_t offset = first; offset < total;) {
        if (sp_stopped(runtime))
            return AZ_CANCELLED;
        uint32_t count = total - offset < AZ_PLAYLIST_PAGE_LIMIT ? total - offset : AZ_PLAYLIST_PAGE_LIMIT;
        SpOrder page = {0};
        az_playlist_entry_v1 *rows = order->rows + offset;
        int status = order_page(runtime, offset, rows, count, &page.identity);
        if (status != AZ_OK)
            return status;
        if (!page_valid(order, &page, offset, count) || !rows_valid(rows, count, offset))
            return AZ_STALE;
        offset += count;
    }
    return AZ_OK;
}

static int first_page(SpRuntime *runtime, az_playlist_entry_v1 *rows, SpOrder *order)
{
    int status = order_page(runtime, 0, rows, AZ_PLAYLIST_PAGE_LIMIT, &order->identity);
    if (status != AZ_OK)
        return status;
    uint32_t total = order->identity.total;
    if (total == 0)
        return AZ_EMPTY;
    if (total > SP_MAX_ENTRIES)
        return AZ_LIMIT;
    uint32_t count = total < AZ_PLAYLIST_PAGE_LIMIT ? total : AZ_PLAYLIST_PAGE_LIMIT;
    if (!page_valid(order, order, 0, count) || !rows_valid(rows, count, 0))
        return AZ_STALE;
    return AZ_OK;
}

int sp_read_order(SpRuntime *runtime, SpOrder *out)
{
    az_playlist_entry_v1 first[AZ_PLAYLIST_PAGE_LIMIT];
    SpOrder order = {0};
    int status = first_page(runtime, first, &order);
    if (status != AZ_OK)
        return status;
    uint32_t total = order.identity.total, count = order.identity.returned;
    order.rows = malloc(total * sizeof *order.rows);
    if (!order.rows)
        return AZ_FAILED;
    memcpy(order.rows, first, count * sizeof *first);
    status = remaining_pages(runtime, &order, count);
    if (status != AZ_OK) {
        sp_clear_order(&order);
        return status;
    }
    *out = order;
    return AZ_OK;
}

int sp_order_check(SpRuntime *runtime)
{
    SpOrder current = {0};
    az_playlist_entry_v1 first;
    int status = order_page(runtime, 0, &first, 1, &current.identity);
    if (status != AZ_OK)
        return status;
    return page_valid(&runtime->model.source, &current, 0, 1) ? AZ_OK : AZ_STALE;
}

typedef struct {
    SpRuntime *runtime;
    uint32_t index;
    int status;
} TrackList;

static double positive(double number)
{
    return isfinite(number) && number > 0 ? number : 0;
}

static void copy_track(SpTrack *to, SpInput *input, const az_track *from)
{
    *to = (SpTrack){.bpm = positive(from->bpm), .duration = positive(from->len_s)};
    *input = (SpInput){0};
    snprintf(to->title, sizeof to->title, "%s", from->title ? from->title : "");
    snprintf(to->artist, sizeof to->artist, "%s", from->artist ? from->artist : "");
    snprintf(to->key_text, sizeof to->key_text, "%s", from->key ? from->key : "");
    if (from->path && strnlen(from->path, sizeof input->path) < sizeof input->path)
        snprintf(input->path, sizeof input->path, "%s", from->path);
    sp_track_key(to, from->key);
}

static int add_track(const az_track *track, void *user)
{
    TrackList *list = user;
    SpModel *model = &list->runtime->model;
    uint32_t index = list->index;
    if (sp_cancelled(list->runtime))
        return list->status = AZ_CANCELLED;
    if (index >= model->source.identity.total || track->id != model->source.rows[index].track_id)
        return list->status = AZ_STALE;
    copy_track(&model->tracks[index], &model->inputs[index], track);
    ++list->index;
    return 0;
}

int sp_refresh_tracks(SpRuntime *runtime)
{
    SpModel *model = &runtime->model;
    TrackList list = {.runtime = runtime};
    int status = runtime->library->lib_tracks(SP_ID, model->stick, model->playlist, add_track, &list);
    if (list.status)
        return list.status;
    if (status < 0)
        return status;
    if (status || list.index != model->source.identity.total)
        return AZ_STALE;
    return sp_order_check(runtime);
}

int sp_read_selection(SpRuntime *runtime)
{
    SpModel *model = &runtime->model;
    sp_discard_selection(model);
    int status = sp_read_order(runtime, &model->source);
    if (status != AZ_OK)
        return status;
    uint32_t count = model->source.identity.total;
    model->tracks = calloc(count, sizeof *model->tracks);
    model->inputs = calloc(count, sizeof *model->inputs);
    status = model->tracks && model->inputs ? sp_refresh_tracks(runtime) : AZ_FAILED;
    if (status != AZ_OK)
        sp_discard_selection(model);
    return status;
}


static int find_name(int id, int parent, const char *name, int folder, void *user)
{
    (void)parent;
    SpModel *model = user;
    if (id != model->playlist || folder)
        return 0;
    if (name)
        snprintf(model->playlist_name, sizeof model->playlist_name, "%s", name);
    return 1;
}

static void target_name(SpRuntime *runtime)
{
    SpModel *model = &runtime->model;
    snprintf(model->playlist_name, sizeof model->playlist_name, "%.*s",
             (int)sizeof runtime->launch.name, runtime->launch.name);
    if (!model->playlist_name[0])
        runtime->library->lib_playlists(SP_ID, model->stick, find_name, model);
}

static bool matches_target(const SpOrder *order, const az_plan_set_source_v1 *source)
{
    return order->identity.source_generation == source->source_generation &&
        order->identity.library_id == source->library_id;
}

int sp_load_target(SpRuntime *runtime)
{
    az_plan_set_source_v1 source = {.size = sizeof source, .version = AZ_DATA_VERSION};
    int status = runtime->core->plan_set_resolve(SP_ID, &runtime->launch, &source);
    if (status != AZ_OK)
        return status;
    SpModel *model = &runtime->model;
    model->stick = (int)source.stick;
    model->playlist = (int)runtime->launch.playlist_id;
    target_name(runtime);
    status = sp_read_selection(runtime);
    if (status == AZ_OK && !matches_target(&model->source, &source)) {
        sp_discard_selection(model);
        return AZ_STALE;
    }
    return status;
}
