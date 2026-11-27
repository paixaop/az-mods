/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"
#include "provider/wire.h"
#include "providers.h"

#include <mbedtls/platform_util.h>
#include <stdio.h>
#include <string.h>

void mcp_token_publish(struct mcp_runtime *runtime, enum mcp_token_status status)
{
    pthread_mutex_lock(&runtime->lock);
    struct mcp_token_page *page = &runtime->token_page;
    page->status = status;
    page->dirty = true;
    pthread_mutex_unlock(&runtime->lock);
}

void mcp_connection_publish(struct mcp_runtime *runtime, const char *address,
                            unsigned port, const char *note)
{
    pthread_mutex_lock(&runtime->lock);
    struct mcp_token_page *page = &runtime->token_page;
    page->url[0] = '\0';
    if (address)
        snprintf(page->url, sizeof page->url, "https://%s:%u/mcp", address, port);
    page->connection_note = note;
    page->dirty = true;
    pthread_mutex_unlock(&runtime->lock);
}

bool mcp_token_requested(struct mcp_runtime *runtime)
{
    pthread_mutex_lock(&runtime->lock);
    bool requested = runtime->token_page.status == MCP_TOKEN_SAVING;
    pthread_mutex_unlock(&runtime->lock);
    return requested;
}

void mcp_pair_offer(struct mcp_runtime *runtime, enum mcp_pair_status status,
                    const char *url, const char *code, const char *fingerprint)
{
    pthread_mutex_lock(&runtime->lock);
    struct mcp_token_page *page = &runtime->token_page;
    mbedtls_platform_zeroize(page->pair_code, sizeof page->pair_code);
    snprintf(page->pair_url, sizeof page->pair_url, "%s", url ? url : "");
    snprintf(page->pair_code, sizeof page->pair_code, "%s", code ? code : "");
    snprintf(page->fingerprint, sizeof page->fingerprint, "%s", fingerprint ? fingerprint : "");
    page->pair_status = status;
    page->dirty = true;
    pthread_mutex_unlock(&runtime->lock);
}

enum mcp_pair_action mcp_pair_take_action(struct mcp_runtime *runtime)
{
    pthread_mutex_lock(&runtime->lock);
    enum mcp_pair_action action = runtime->token_page.pair_action;
    runtime->token_page.pair_action = MCP_PAIR_NONE;
    pthread_mutex_unlock(&runtime->lock);
    return action;
}

struct page_snapshot {
    char url[MCP_URL_SIZE];
    char pair_url[MCP_URL_SIZE], pair_code[MCP_PAIR_CODE_SIZE], fingerprint[MCP_FINGERPRINT_SIZE];
    const char *note;
    enum mcp_token_status status;
    enum mcp_pair_status pair_status;
};

static void read_page(struct mcp_runtime *runtime, struct page_snapshot *snapshot)
{
    pthread_mutex_lock(&runtime->lock);
    const struct mcp_token_page *page = &runtime->token_page;
    snapshot->status = page->status;
    snapshot->note = page->connection_note;
    snapshot->pair_status = page->pair_status;
    memcpy(snapshot->url, page->url, sizeof snapshot->url);
    memcpy(snapshot->pair_url, page->pair_url, sizeof snapshot->pair_url);
    memcpy(snapshot->pair_code, page->pair_code, sizeof snapshot->pair_code);
    memcpy(snapshot->fingerprint, page->fingerprint, sizeof snapshot->fingerprint);
    pthread_mutex_unlock(&runtime->lock);
}

static int text_width(const az_core_v1 *core, const char *label, int px)
{
    return AZ_HAS(core, text_width) ? core->text_width(label, px) : (int)strlen(label) * px;
}

static void fit_text(const az_core_v1 *core, az_draw *draw, az_rect area, int y,
                     const char *label, int color)
{
    int px = 14;
    while (px > 8 && text_width(core, label, px) > area.w)
        --px;
    core->draw_text(draw, area.x, area.y + y, label, px, core->color(color));
}

static void button(struct mcp_runtime *runtime, az_draw *draw, az_rect area, int y,
                    enum mcp_screen_button which, const char *label)
{
    const az_core_v1 *core = runtime->core;
    if (y + 44 > area.h)
        return;
    area.y += y;
    area.w = area.w < 222 ? area.w : 222;
    area.h = 44;
    runtime->token_page.buttons[which] = area;
    core->fill_rect(draw, area.x, area.y, area.w, area.h, core->color(AZ_COL_HI));
    az_rect inset = { area.x + 8, area.y, area.w - 16, area.h };
    fit_text(core, draw, inset, 11, label, AZ_COL_HI_TEXT);
}

static size_t token_columns(const az_core_v1 *core, int width, const char *token)
{
    if (text_width(core, token, 14) <= width)
        return 64;
    char first_half[33];
    memcpy(first_half, token, 32);
    first_half[32] = '\0';
    bool fits = text_width(core, first_half, 14) <= width && text_width(core, token + 32, 14) <= width;
    mbedtls_platform_zeroize(first_half, sizeof first_half);
    return fits ? 32 : 16;
}

static void draw_secret(const az_core_v1 *core, az_draw *draw, az_rect area, const char *token,
                        size_t columns)
{
    char line[MCP_SECRET_SIZE];
    line[columns] = '\0';
    for (size_t offset = 0; offset < MCP_SECRET_SIZE - 1; offset += columns) {
        memcpy(line, token + offset, columns);
        fit_text(core, draw, area, 24 * (int)(offset / columns), line, AZ_COL_TEXT);
    }
    mbedtls_platform_zeroize(line, sizeof line);
}

static bool live_armed(const struct mcp_runtime *runtime)
{
    return mcp_live_grant_generation(runtime, az_mcp_clock()) != 0;
}

static bool saved_armed(const struct mcp_runtime *runtime)
{
    return mcp_saved_grant_generation(runtime, az_mcp_clock()) != 0;
}

static void draw_arming(struct mcp_runtime *runtime, az_draw *draw, az_rect area, int y)
{
    struct mcp_token_page *page = &runtime->token_page;
    page->shown_live = live_armed(runtime);
    page->shown_saved = saved_armed(runtime);
    if (runtime->commands.api)
        button(runtime, draw, area, y, MCP_BUTTON_LIVE, page->shown_live ? "Disarm live controls" : "Arm live controls");
    button(runtime, draw, area, y + 48, MCP_BUTTON_SAVED, page->shown_saved ? "Disarm saved edits" : "Arm saved edits");
}

static bool can_reset_token(enum mcp_token_status status)
{
    return status != MCP_TOKEN_UNAVAILABLE && status != MCP_TOKEN_SAVING;
}

static void draw_home(struct mcp_runtime *runtime, az_draw *draw, az_rect area,
                      const struct page_snapshot *snapshot)
{
    const az_core_v1 *core = runtime->core;
    if (!snapshot->url[0]) {
        fit_text(core, draw, area, 0, snapshot->note ? snapshot->note : "Preparing Deck MCP...", AZ_COL_TEXT);
        return;
    }
    fit_text(core, draw, area, 0, "Harness URL", AZ_COL_TEXT_DIM);
    fit_text(core, draw, area, 24, snapshot->url, AZ_COL_TEXT);
    if (area.h < 74)
        return;
    fit_text(core, draw, area, 48, "Pair with Codex using your laptop browser.", AZ_COL_TEXT);
    button(runtime, draw, area, 80, MCP_BUTTON_PAIR, "Pair with Codex");
    draw_arming(runtime, draw, area, 128);
    if (can_reset_token(snapshot->status))
        button(runtime, draw, area, 232, MCP_BUTTON_RESET, "Reset token");
}

static const char *pair_message(const struct page_snapshot *snapshot)
{
    if (snapshot->status == MCP_TOKEN_SAVING)
        return "Saving the new token...";
    if (snapshot->status == MCP_TOKEN_FAILED && snapshot->pair_status == MCP_PAIR_IDLE)
        return "Save failed. The previous token still works.";
    static const char *const messages[] = {
        [MCP_PAIR_UNAVAILABLE] = "Could not generate a setup code.",
        [MCP_PAIR_IDLE] = "Generate a setup code.",
        [MCP_PAIR_PREPARING] = "Generating a one-time code...",
        [MCP_PAIR_READY] = "Open this address in your laptop browser.",
        [MCP_PAIR_USED] = "Connection file sent. Open it with your local coding assistant.",
        [MCP_PAIR_EXPIRED] = "Code expired. Generate a new one.",
        [MCP_PAIR_LOCKED] = "Too many attempts. Generate a new code."
    };
    return messages[snapshot->pair_status];
}

static void draw_pair_details(const az_core_v1 *core, az_draw *draw, az_rect area,
                              const struct page_snapshot *snapshot)
{
    fit_text(core, draw, area, 120, snapshot->pair_url, AZ_COL_TEXT);
    char prompt[56];
    snprintf(prompt, sizeof prompt, "Setup code: %s (valid for 2 minutes)", snapshot->pair_code);
    fit_text(core, draw, area, 144, prompt, AZ_COL_TEXT);
    mbedtls_platform_zeroize(prompt, sizeof prompt);
    if (area.h < 192)
        return;
    fit_text(core, draw, area, 168, "If warned, verify the certificate SHA-256 fingerprint.", AZ_COL_TEXT_DIM);
    size_t columns = token_columns(core, area.w, snapshot->fingerprint);
    if (area.h >= 192 + 24 * (int)(64 / columns))
        draw_secret(core, draw, (az_rect){ area.x, area.y + 192, area.w, area.h - 192 },
                    snapshot->fingerprint, columns);
}

static void draw_pair(struct mcp_runtime *runtime, az_draw *draw, az_rect area,
                      const struct page_snapshot *snapshot)
{
    const az_core_v1 *core = runtime->core;
    button(runtime, draw, area, 0, MCP_BUTTON_BACK, "Back");
    button(runtime, draw, area, 48, MCP_BUTTON_PAIR, "New setup code");
    if (area.h < 120)
        return;
    fit_text(core, draw, area, 96, pair_message(snapshot), AZ_COL_TEXT);
    if (snapshot->pair_status == MCP_PAIR_READY && snapshot->status != MCP_TOKEN_SAVING && area.h >= 167)
        draw_pair_details(core, draw, area, snapshot);
}

static void draw_page(az_draw *draw, az_rect area, void *user)
{
    struct mcp_runtime *runtime = user;
    struct mcp_token_page *page = &runtime->token_page;
    memset(page->buttons, 0, sizeof page->buttons);
    if (area.w < 184 || area.h < 22)
        return;
    struct page_snapshot snapshot;
    read_page(runtime, &snapshot);
    if (page->page == MCP_SCREEN_PAIR)
        draw_pair(runtime, draw, area, &snapshot);
    else
        draw_home(runtime, draw, area, &snapshot);
    mbedtls_platform_zeroize(&snapshot, sizeof snapshot);
}

static bool hit(az_rect area, const az_event *event)
{
    return event->x >= area.x && event->x < area.x + area.w &&
           event->y >= area.y && event->y < area.y + area.h;
}

static void request_pair(struct mcp_runtime *runtime, enum mcp_pair_action action)
{
    struct mcp_token_page *page = &runtime->token_page;
    pthread_mutex_lock(&runtime->lock);
    page->pair_action = action;
    page->pair_status = action == MCP_PAIR_START ? MCP_PAIR_PREPARING : MCP_PAIR_IDLE;
    page->pair_url[0] = '\0';
    mbedtls_platform_zeroize(page->pair_code, sizeof page->pair_code);
    page->fingerprint[0] = '\0';
    pthread_mutex_unlock(&runtime->lock);
}

static void leave_page(struct mcp_runtime *runtime)
{
    if (runtime->token_page.page == MCP_SCREEN_PAIR)
        request_pair(runtime, MCP_PAIR_CANCEL);
    runtime->token_page.page = MCP_SCREEN_HOME;
}

static void regenerate_confirmed(int confirmed, void *user)
{
    if (!confirmed)
        return;
    struct mcp_runtime *runtime = user;
    mcp_commands_disarm(runtime);
    mcp_saved_disarm(runtime);
    pthread_mutex_lock(&runtime->lock);
    struct mcp_token_page *page = &runtime->token_page;
    if (can_reset_token(page->status)) {
        page->status = MCP_TOKEN_SAVING;
        page->pair_action = MCP_PAIR_NONE;
        page->page = MCP_SCREEN_PAIR;
    }
    pthread_mutex_unlock(&runtime->lock);
    runtime->core->request_redraw();
}

static void toggle_live(struct mcp_runtime *runtime)
{
    if (runtime->token_page.shown_live)
        mcp_commands_disarm(runtime);
    else
        mcp_commands_arm(runtime);
}

static void toggle_saved(struct mcp_runtime *runtime)
{
    if (runtime->token_page.shown_saved)
        mcp_saved_disarm(runtime);
    else
        mcp_saved_arm(runtime);
}

static void activate(struct mcp_runtime *runtime, enum mcp_screen_button which)
{
    switch (which) {
    case MCP_BUTTON_PAIR:
        runtime->token_page.page = MCP_SCREEN_PAIR;
        request_pair(runtime, MCP_PAIR_START);
        break;
    case MCP_BUTTON_BACK:
        leave_page(runtime);
        break;
    case MCP_BUTTON_LIVE:
        toggle_live(runtime);
        break;
    case MCP_BUTTON_SAVED:
        toggle_saved(runtime);
        break;
    case MCP_BUTTON_RESET:
        runtime->core->confirm(MCP_ID, "Reset MCP token?",
            "This signs out current clients. Pair again to reconnect.", "Reset token", regenerate_confirmed, runtime);
        break;
    case MCP_BUTTONS:
        break;
    }
    memset(runtime->token_page.buttons, 0, sizeof runtime->token_page.buttons);
    runtime->core->request_redraw();
}

static int tap_button(struct mcp_runtime *runtime, const az_event *event)
{
    for (enum mcp_screen_button which = 0; which < MCP_BUTTONS; ++which) {
        if (hit(runtime->token_page.buttons[which], event)) {
            activate(runtime, which);
            return 1;
        }
    }
    return 0;
}

static int page_input(const az_event *event, void *user)
{
    struct mcp_runtime *runtime = user;
    if (event->kind == AZ_EV_TAP)
        return tap_button(runtime, event);
    if (event->kind == AZ_EV_BACK && runtime->token_page.page != MCP_SCREEN_HOME) {
        activate(runtime, MCP_BUTTON_BACK);
        return 1;
    }
    return 0;
}

static void page_enter(void *user)
{
    struct mcp_runtime *runtime = user;
    leave_page(runtime);
    memset(runtime->token_page.buttons, 0, sizeof runtime->token_page.buttons);
}

static bool supports_screen(const az_core_v1 *core)
{
    return AZ_HAS(core, register_screen) && AZ_HAS(core, draw_text) &&
           AZ_HAS(core, fill_rect) && AZ_HAS(core, color) &&
           AZ_HAS(core, request_redraw) && AZ_HAS(core, confirm);
}

void mcp_token_screen_init(struct mcp_runtime *runtime)
{
    if (!supports_screen(runtime->core))
        return;
    static az_screen_v1 page = {
        .size = sizeof page, .on_draw = draw_page, .on_input = page_input,
        .on_enter = page_enter, .flags = AZ_SCREEN_DEDICATED
    };
    page.user = runtime;
    runtime->core->register_screen(MCP_ID, &page);
}

static bool arming_changed(const struct mcp_runtime *runtime)
{
    const struct mcp_token_page *page = &runtime->token_page;
    return (page->buttons[MCP_BUTTON_LIVE].h && page->shown_live != live_armed(runtime)) ||
           (page->buttons[MCP_BUTTON_SAVED].h && page->shown_saved != saved_armed(runtime));
}

void mcp_token_screen_tick(struct mcp_runtime *runtime)
{
    pthread_mutex_lock(&runtime->lock);
    bool dirty = runtime->token_page.dirty;
    runtime->token_page.dirty = false;
    pthread_mutex_unlock(&runtime->lock);
    if ((dirty || arming_changed(runtime)) && AZ_HAS(runtime->core, request_redraw))
        runtime->core->request_redraw();
}
