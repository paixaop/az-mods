/* SPDX-License-Identifier: GPL-3.0-only */
#include "data-tools.h"

#include <string.h>

#define LIVE_GRID_LIMIT 8192u
#define LIVE_PHRASE_LIMIT 1024u
#define LIVE_PAGE_LIMIT 128u

enum { CURRENT_GRID, SAVED_GRID, CURRENT_PHRASES, SAVED_PHRASES, LIVE_BANKS };
struct live_info {
    az_status_field_info_v1 receipt;
    az_status_grid_ref_v1 grid;
    uint32_t count;
};
struct live_bank {
    struct live_info info;
    union {
        az_beat beats[LIVE_GRID_LIMIT];
        az_phrase phrases[LIVE_PHRASE_LIMIT];
    } rows;
};
struct live_deck {
    uint64_t epoch;
    struct live_bank banks[LIVE_BANKS];
};

static struct live_deck ui[MCP_DECKS], published[MCP_DECKS];
static unsigned changed[MCP_DECKS];

void mcp_live_reset(void)
{
    memset(ui, 0, sizeof ui);
    memset(published, 0, sizeof published);
    memset(changed, 0, sizeof changed);
}

static bool same_collection(const struct live_info *a, const struct live_info *b)
{
    return a->receipt.scope_id == b->receipt.scope_id && a->receipt.provider_id == b->receipt.provider_id &&
           a->receipt.info.revision == b->receipt.info.revision &&
           a->receipt.info.availability == b->receipt.info.availability &&
           a->receipt.info.provenance == b->receipt.info.provenance &&
           a->receipt.count == b->receipt.count && a->count == b->count &&
           !memcmp(&a->grid, &b->grid, sizeof a->grid);
}

static bool receipt_loaded(const az_status_field_info_v1 *receipt, uint64_t epoch)
{
    return epoch && receipt->scope == AZ_STATUS_LOAD_SCOPE && receipt->scope_id == epoch;
}

static bool topic_ready(const az_status_topic_v1 *header, size_t size)
{
    return header && header->size >= size && header->version == AZ_DATA_VERSION;
}

static struct live_info collection(const az_status_field_info_v1 *receipt,
                                   const void *rows, uint64_t epoch, uint32_t limit)
{
    struct live_info info = {0};
    if (!receipt_loaded(receipt, epoch))
        return info;
    info.receipt = *receipt;
    if (receipt->info.availability == AZ_AVAIL_READY) {
        if (rows)
            info.count = receipt->count < limit ? receipt->count : limit;
        else if (receipt->count)
            info.receipt.info.availability = AZ_AVAIL_STALE;
    }
    return info;
}

static const void *ready_rows(const az_status_topic_v1 *header, unsigned field, const void *rows)
{
    return header->valid & (UINT64_C(1) << field) ? rows : NULL;
}

static void update_bank(unsigned deck, unsigned index, struct live_info info, const void *rows,
                            size_t row_size, bool replace)
{
    struct live_bank *bank = &ui[deck].banks[index];
    if (!replace && same_collection(&bank->info, &info))
        return;
    bank->info = info;
    if (info.count)
        memcpy(&bank->rows, rows, info.count * row_size);
    changed[deck] |= 1u << index;
}

static void capture_grids(const az_deck_status_event_v1 *event, const az_status_grid_v1 *grid, bool replace)
{
    struct live_info current = {0}, saved = {0};
    if (!topic_ready(grid ? &grid->header : NULL, sizeof *grid))
        grid = NULL;
    if (grid) {
        current = collection(&grid->current_info, ready_rows(&grid->header, AZ_STATUS_GRID_CURRENT, grid->current),
                             event->load_epoch, LIVE_GRID_LIMIT);
        saved = collection(&grid->saved_info, ready_rows(&grid->header, AZ_STATUS_GRID_SAVED, grid->saved),
                           event->load_epoch, LIVE_GRID_LIMIT);
    }
    update_bank(event->deck, CURRENT_GRID, current, grid ? grid->current : NULL, sizeof(az_beat), replace);
    update_bank(event->deck, SAVED_GRID, saved, grid ? grid->saved : NULL, sizeof(az_beat), replace);
}

static void link_phrase_grid(struct live_info *info, const az_status_field_info_v1 *receipt,
                             const az_status_grid_ref_v1 *grid, uint64_t epoch)
{
    if (grid && receipt_loaded(receipt, epoch) && receipt->info.availability == AZ_AVAIL_READY &&
        grid->load_epoch == epoch && grid->revision && grid->provider_id) {
        info->grid = *grid;
        return;
    }
    if (info->count) {
        info->count = 0;
        info->receipt.info.availability = AZ_AVAIL_STALE;
    }
}

static void capture_phrases(const az_deck_status_event_v1 *event, const az_status_phrases_v1 *phrases,
                             bool replace)
{
    struct live_info current = {0}, saved = {0};
    if (!topic_ready(phrases ? &phrases->header : NULL, sizeof *phrases))
        phrases = NULL;
    if (phrases) {
        uint64_t epoch = event->load_epoch;
        const az_status_topic_v1 *h = &phrases->header;
        current = collection(&phrases->current_info, ready_rows(h, AZ_STATUS_PHRASES_CURRENT, phrases->current),
                             epoch, LIVE_PHRASE_LIMIT);
        saved = collection(&phrases->saved_info, ready_rows(h, AZ_STATUS_PHRASES_SAVED, phrases->saved),
                           epoch, LIVE_PHRASE_LIMIT);
        link_phrase_grid(&current, &phrases->current_grid_info,
                         ready_rows(h, AZ_STATUS_PHRASES_CURRENT_GRID, &phrases->current_grid), epoch);
        link_phrase_grid(&saved, &phrases->saved_grid_info,
                         ready_rows(h, AZ_STATUS_PHRASES_SAVED_GRID, &phrases->saved_grid), epoch);
    }
    update_bank(event->deck, CURRENT_PHRASES, current, phrases ? phrases->current : NULL, sizeof(az_phrase), replace);
    update_bank(event->deck, SAVED_PHRASES, saved, phrases ? phrases->saved : NULL, sizeof(az_phrase), replace);
}

void mcp_live_capture(const az_deck_status_event_v1 *event)
{
    const az_deck_status_view_v1 *view = event->view;
    const az_status_grid_v1 *grid = view->size >= offsetof(az_deck_status_view_v1, phrases) ? view->grid : NULL;
    const az_status_phrases_v1 *phrases = view->size >= offsetof(az_deck_status_view_v1, transport) ? view->phrases : NULL;
    struct live_deck *deck = &ui[event->deck];
    bool replace = event->load_epoch != deck->epoch || event->kind != AZ_STATUS_CHANGE;
    deck->epoch = event->load_epoch;
    capture_grids(event, grid, replace);
    capture_phrases(event, phrases, replace);
}

static size_t live_row_size(unsigned bank)
{
    return bank < CURRENT_PHRASES ? sizeof(az_beat) : sizeof(az_phrase);
}

void mcp_live_publish(void)
{
    for (unsigned deck = 0; deck < MCP_DECKS; ++deck) {
        if (!changed[deck])
            continue;
        published[deck].epoch = ui[deck].epoch;
        for (unsigned bank = 0; bank < LIVE_BANKS; ++bank) {
            if (!(changed[deck] & (1u << bank)))
                continue;
            const struct live_bank *source = &ui[deck].banks[bank];
            struct live_bank *dest = &published[deck].banks[bank];
            dest->info = source->info;
            memcpy(&dest->rows, &source->rows, source->info.count * live_row_size(bank));
        }
        changed[deck] = 0;
    }
}

struct live_page {
    struct live_info info;
    uint32_t count;
    union {
        az_beat beats[LIVE_PAGE_LIMIT];
        az_phrase phrases[LIVE_PAGE_LIMIT];
    } rows;
};

static bool page_current(const struct live_deck *deck, const struct live_bank *bank,
                           const cmcp_json_t *args, uint32_t offset)
{
    uint64_t epoch = mcp_uint64_arg(args, "loadEpoch");
    if (!epoch || deck->epoch != epoch)
        return false;
    uint64_t revision = mcp_uint64_arg(args, "revision");
    uint64_t provider = mcp_uint64_arg(args, "providerId");
    if (!offset && !revision && !provider)
        return true;
    return revision && provider && revision == bank->info.receipt.info.revision &&
           provider == bank->info.receipt.provider_id;
}

static bool live_page_copy(struct mcp_runtime *runtime, unsigned deck_no, unsigned index,
                             uint32_t offset, const cmcp_json_t *args, struct live_page *page)
{
    pthread_mutex_lock(&runtime->lock);
    const struct live_deck *deck = &published[deck_no - 1];
    const struct live_bank *bank = &deck->banks[index];
    bool current = page_current(deck, bank, args, offset);
    if (current) {
        page->info = bank->info;
        uint32_t remaining = offset < bank->info.count ? bank->info.count - offset : 0;
        uint32_t limit = mcp_page_limit(args);
        page->count = remaining < limit ? remaining : limit;
        size_t stride = live_row_size(index);
        if (page->count)
            memcpy(&page->rows, (const char *)&bank->rows + offset * stride, page->count * stride);
    }
    pthread_mutex_unlock(&runtime->lock);
    return current;
}

static cmcp_json_t *grid_reference(const az_status_grid_ref_v1 *grid)
{
    cmcp_json_t *out = cmcp_json_new_object();
    mcp_set(out, "loadEpoch", mcp_id(grid->load_epoch));
    mcp_set(out, "providerId", mcp_id(grid->provider_id));
    mcp_set(out, "revision", mcp_id(grid->revision));
    mcp_set(out, "provenance", cmcp_json_new_int(grid->provenance));
    mcp_set(out, "variant", cmcp_json_new_string(grid->grid_field == AZ_STATUS_GRID_SAVED ? "saved" : "current"));
    return out;
}

static void live_result(cmcp_json_t *out, const struct live_page *page, unsigned bank, uint32_t offset)
{
    mcp_set(out, "receipt", mcp_receipt_json(&page->info.receipt));
    mcp_set(out, "total", cmcp_json_new_int(page->info.receipt.count));
    mcp_set(out, "truncated", cmcp_json_new_bool(page->info.count < page->info.receipt.count &&
                                               page->info.receipt.info.availability == AZ_AVAIL_READY));
    if (page->info.grid.revision)
        mcp_set(out, "grid", grid_reference(&page->info.grid));
    cmcp_json_t *rows = cmcp_json_new_array();
    for (uint32_t i = 0; i < page->count; ++i)
        mcp_sdk_append(rows, bank < CURRENT_PHRASES ? mcp_beat_json(&page->rows.beats[i])
                                                   : mcp_phrase_json(&page->rows.phrases[i]));
    mcp_set(out, "rows", rows);
    uint32_t next = offset + page->count;
    mcp_set(out, "nextOffset", next < page->info.count ? cmcp_json_new_int(next) : cmcp_json_new_null());
}

static unsigned live_bank_arg(const cmcp_json_t *args)
{
    const char *component = az_mcp_string(args, "component");
    const char *variant = az_mcp_string(args, "variant");
    unsigned index = component && !strcmp(component, "phrases") ? CURRENT_PHRASES : CURRENT_GRID;
    return variant && !strcmp(variant, "saved") ? index + 1 : index;
}

static int live_tool(const cmcp_json_t *args, void *user, cmcp_handler_ctx_t *ctx,
                       cmcp_json_t **content, int *is_error)
{
    struct mcp_runtime *runtime = user;
    unsigned deck = mcp_input_deck(args);
    if (!deck)
        return mcp_tool_error("Deck must be in the range 1 to 4.", content, is_error);
    uint32_t offset = (uint32_t)az_mcp_number(args, "offset");
    unsigned bank = live_bank_arg(args);
    struct live_page page;
    bool current = live_page_copy(runtime, deck, bank, offset, args, &page);
    cmcp_json_t *out = mcp_sdk_result(runtime, current ? AZ_OK : AZ_STALE);
    if (current)
        live_result(out, &page, bank, offset);
    return mcp_tool_result(ctx, out, content);
}

int mcp_register_live_tools(cmcp_server_t *server, struct mcp_runtime *runtime)
{
    const cmcp_tool_t tool = { .name = "deck_read_live_analysis",
        .description = "Read the current or saved beatgrid or phrase map for a loaded deck. Use loadEpoch and carry the receipt's revision and providerId to later pages.",
        .input_schema = MCP_SCHEMA(MCP_ARG_DECK "," MCP_ARG_EPOCH "," MCP_ARG_OFFSET "," MCP_ARG_LIMIT ","
            "\"component\":{\"enum\":[\"beatGrid\",\"phrases\"]},\"variant\":{\"enum\":[\"current\",\"saved\"]},"
            "\"revision\":{\"type\":\"string\",\"maxLength\":20},"
            "\"providerId\":{\"type\":\"string\",\"maxLength\":20}",
            "\"deck\",\"loadEpoch\",\"component\""),
        .handler = live_tool, .userdata = runtime };
    return cmcp_server_add_tool(server, &tool);
}
