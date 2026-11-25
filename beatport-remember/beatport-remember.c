/* SPDX-License-Identifier: GPL-3.0-only
 * Beatport Remember configures the public SDK login service. The core owns
 * native UI access, screen lifetime, credential capture and durable storage.
 * This mod never receives the password and does no filesystem I/O.
 */
#include "azmod-mod.h"
#ifdef BEATPORT_MCP
#include "wire.h"
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MOD_ID "beatport-remember"
#define DEFAULT_DELAY_SEC 45
#define DEFAULT_ATTEMPTS 1

static const az_core_v1 *C;
static const az_beatport_login_v1 *login_service;
static az_beatport_login_options options;
static az_beatport_login_status login_status;
static int status_available;
static const char *action_error;
static int status_ticks;

static void redraw(void)
{
    if (AZ_HAS(C, request_redraw)) C->request_redraw();
}

/* Called only from overlay callbacks. A snapshot is a copy; the core never
 * exposes a firmware object or its storage worker's mutable state here. */
static int refresh_status(void)
{
    az_beatport_login_status next = { .size = sizeof(next) };
    int available = login_service->status(MOD_ID, &next) == 0;
    int changed;

    next.username[sizeof(next.username) - 1] = 0;
    changed = available != status_available ||
              next.have_credentials != login_status.have_credentials ||
              next.save_pending != login_status.save_pending ||
              next.storage_error != login_status.storage_error ||
              strcmp(next.username, login_status.username);
    login_status = next;
    status_available = available;
    return changed;
}

static const char *status_line(void)
{
    static char line[176];
    if (action_error) return action_error;
    if (!status_available) return "Saved login status unavailable.";
    if (login_status.save_pending) return "Updating saved login...";
    if (login_status.storage_error) return "Could not update saved login. Check storage and retry.";
    if (!login_status.have_credentials) return "No saved login. Log in once on the Beatport screen.";
    snprintf(line, sizeof(line), "Saved login: %s", login_status.username);
    return line;
}

static az_rect g_auto_row, g_rem_row, g_forget_btn;

static int rect_hit(const az_rect *r, const az_event *ev)
{
    return r->w > 0 && r->h > 0 &&
           ev->x >= r->x && ev->x < r->x + r->w &&
           ev->y >= r->y && ev->y < r->y + r->h;
}

static size_t utf8_prefix(const char *text, size_t length)
{
    while (length && ((unsigned char)text[length] & 0xc0) == 0x80) length--;
    return length;
}

static void draw_fitted(az_draw *d, int x, int y, int width, const char *text,
                        int px, uint32_t rgb)
{
    char clipped[192];
    size_t n;
    if (width <= 0) return;
    if (C->text_width(text, px) <= width) {
        C->draw_text(d, x, y, text, px, rgb);
        return;
    }
    if (C->text_width("...", px) > width) return;
    n = strlen(text);
    if (n > sizeof(clipped) - 4) n = sizeof(clipped) - 4;
    n = utf8_prefix(text, n);
    memcpy(clipped, text, n);
    for (;;) {
        memcpy(clipped + n, "...", 4);
        if (C->text_width(clipped, px) <= width) break;
        if (!n) return;
        n = utf8_prefix(clipped, n - 1);
    }
    C->draw_text(d, x, y, clipped, px, rgb);
}

static void draw_toggle(az_draw *d, az_rect area, int y, int row, int on,
                        const char *label, az_rect *out)
{
    int px = row / 2;
    int cb = row / 2;
    if (px < 8) px = 8;
    if (px > 96) px = 96;
    if (cb < 12) cb = 12;
    *out = (az_rect){ 0, 0, 0, 0 };
    if (y + row > area.y + area.h || row / 2 + cb > area.w) return;
    C->draw_checkbox(d, area.x + row / 2, y + (row - cb) / 2, cb, on, 0);
    draw_fitted(d, area.x + row + cb, y + (row - px * 3 / 2) / 2,
                area.w - row - cb - row / 2, label, px, C->color(AZ_COL_TEXT));
    *out = (az_rect){ area.x, y, area.w, row };
}

static void draw_forget(az_draw *d, az_rect area, int y, int row, int px)
{
    /* Keep Forget available after a failed update, including a failed delete.
     * The service decides which durable/in-memory values still need clearing. */
    if (status_available && (login_status.have_credentials || login_status.save_pending ||
                             login_status.storage_error)) {
        int tw = C->text_width("Forget saved login", px);
        int bw = tw + row;
        int bh = row * 3 / 4;
        int bx = area.x + row / 2;
        if (tw > 0 && bw <= area.w - row && y + bh <= area.y + area.h) {
            C->fill_rect(d, bx, y, bw, bh, C->color(AZ_COL_HI));
            C->draw_text(d, bx + row / 2, y + (bh - px * 3 / 2) / 2,
                         "Forget saved login", px, C->color(AZ_COL_HI_TEXT));
            g_forget_btn = (az_rect){ bx, y, bw, bh };
        }
    }
}

static void draw_page(az_draw *d, az_rect area, void *user)
{
    const int row = C->row_height(d);
    const int px = row / 2 < 8 ? 8 : (row / 2 > 96 ? 96 : row / 2);
    int y;
    (void)user;
    C->draw_header(d, "Beatport Remember");
    g_auto_row = g_rem_row = g_forget_btn = (az_rect){ 0, 0, 0, 0 };
    y = area.y + row + row / 4;
    if (y + px * 3 / 2 <= area.y + area.h)
        draw_fitted(d, area.x + row / 2, y, area.w - row, status_line(), px,
                    C->color(AZ_COL_TEXT_DIM));
    y += row + row / 4;
    draw_toggle(d, area, y, row, options.auto_login, "Auto login", &g_auto_row);
    y += row;
    draw_toggle(d, area, y, row, options.remember, "Remember a login typed on the deck", &g_rem_row);
    y += row + row / 2;

    draw_forget(d, area, y, row, px);
}

static int change_option(const char *key, int automatic)
{
    az_beatport_login_options next = options;
    int value;
    if (automatic) value = next.auto_login = !next.auto_login;
    else value = next.remember = !next.remember;
    if (login_service->configure(MOD_ID, &next) != 0) {
        action_error = "Could not change login settings. Try again.";
        C->log(MOD_ID, "login service rejected a settings change");
    } else {
        options = next;
        action_error = NULL;
        C->cfg_set(MOD_ID, key, value);
    }
    redraw();
    return 1;
}

static int page_input(const az_event *ev, void *user)
{
    (void)user;
    if (!ev || ev->kind != AZ_EV_TAP) return 0;
    if (rect_hit(&g_auto_row, ev)) return change_option("auto-login", 1);
    if (rect_hit(&g_rem_row, ev)) return change_option("remember", 0);
    if (rect_hit(&g_forget_btn, ev)) {
        if (login_service->forget(MOD_ID) != 0) {
            action_error = "Could not forget saved login. Try again.";
            C->log(MOD_ID, "login service rejected Forget saved login");
        } else {
            action_error = NULL;
            refresh_status();
        }
        redraw();
        return 1;
    }
    return 0;
}

static void page_enter(void *user)
{
    (void)user;
    action_error = NULL;
    refresh_status();
}

static const az_screen_v1 PAGE = {
    .size = sizeof(az_screen_v1),
    .on_draw = draw_page,
    .on_input = page_input,
    .on_enter = page_enter,
};

static int login_contract_available(void)
{
    return login_service && login_service->abi == AZ_BEATPORT_LOGIN_ABI &&
           login_service->size >= offsetof(az_beatport_login_v1, forget) + sizeof(login_service->forget) &&
           login_service->configure && login_service->status && login_service->forget;
}

static void load_options(void)
{
    options = (az_beatport_login_options){ .size = sizeof(options),
                                           .auto_login = !!C->cfg_get(MOD_ID, "auto-login", 1),
                                           .remember = !!C->cfg_get(MOD_ID, "remember", 1),
                                           .delay_sec = C->cfg_get(MOD_ID, "delay-sec", DEFAULT_DELAY_SEC),
                                           .max_attempts = C->cfg_get(MOD_ID, "attempts", DEFAULT_ATTEMPTS) };
    if (options.delay_sec < 0) options.delay_sec = 0;
    if (options.delay_sec > 600) options.delay_sec = 600;
    if (options.max_attempts < 1) options.max_attempts = 1;
    if (options.max_attempts > 4) options.max_attempts = 4;
}

static int core_features_ready(void)
{
    return AZ_HAS(C, beatport_login_service) && AZ_HAS(C, register_row) && AZ_HAS(C, register_screen);
}

#ifdef BEATPORT_MCP
static az_mcp_provider *mcp_provider;

static void mcp_int(cmcp_json_t *out, const char *key, int value)
{
    az_mcp_set(out, key, cmcp_json_new_int(value));
}

static int mcp_status(const cmcp_json_t *args, uint64_t call, uint64_t deadline,
                      cmcp_json_t **out, void *user)
{
    (void)args;
    (void)call;
    (void)deadline;
    (void)user;
    refresh_status();
    if (!status_available)
        return AZ_FAILED;
    *out = cmcp_json_new_object();
    az_mcp_set(*out, "haveCredentials", cmcp_json_new_bool(login_status.have_credentials));
    az_mcp_set(*out, "savePending", cmcp_json_new_bool(login_status.save_pending));
    mcp_int(*out, "storageError", login_status.storage_error);
    mcp_int(*out, "autoLogin", options.auto_login);
    mcp_int(*out, "remember", options.remember);
    mcp_int(*out, "delaySeconds", options.delay_sec);
    mcp_int(*out, "attempts", options.max_attempts);
    return AZ_OK;
}

static void apply_option(const cmcp_json_t *args, const char *key, int *value)
{
    const cmcp_json_t *node = cmcp_json_object_get(args, key);
    if (node && node->type == CMCP_JSON_INT)
        *value = (int)node->i;
}

static int mcp_options(const cmcp_json_t *args, uint64_t call, uint64_t deadline,
                       cmcp_json_t **out, void *user)
{
    az_beatport_login_options next = options;
    apply_option(args, "autoLogin", &next.auto_login);
    apply_option(args, "remember", &next.remember);
    apply_option(args, "delaySeconds", &next.delay_sec);
    apply_option(args, "attempts", &next.max_attempts);
    if (login_service->configure(MOD_ID, &next))
        return AZ_FAILED;
    options = next;
    C->cfg_set(MOD_ID, "auto-login", options.auto_login);
    C->cfg_set(MOD_ID, "remember", options.remember);
    C->cfg_set(MOD_ID, "delay-sec", options.delay_sec);
    C->cfg_set(MOD_ID, "attempts", options.max_attempts);
    action_error = NULL;
    redraw();
    return mcp_status(args, call, deadline, out, user);
}

static void mcp_confirm_forget(int yes, void *user)
{
    (void)user;
    if (!yes)
        return;
    action_error = login_service->forget(MOD_ID) ? "Could not forget saved login." : NULL;
    refresh_status();
    redraw();
}

static int mcp_forget(const cmcp_json_t *args, uint64_t call, uint64_t deadline,
                      cmcp_json_t **out, void *user)
{
    (void)args;
    (void)call;
    (void)deadline;
    (void)user;
    if (!AZ_HAS(C, confirm) || C->confirm(MOD_ID, "Forget Beatport login?",
            "Delete the Beatport login saved on this deck?", "Forget", mcp_confirm_forget, NULL))
        return AZ_BUSY;
    *out = cmcp_json_parse_cstr("{\"confirmationRequested\":true}");
    return *out ? AZ_OK : AZ_FAILED;
}

static void register_mcp(void)
{
    static const az_mcp_tool_v1 tools[] = {
        { .size = sizeof(az_mcp_tool_v1), .name = "status", .description = "Read saved login status and options.",
          .input_schema = "{\"type\":\"object\",\"additionalProperties\":false}", .invoke = mcp_status },
        { .size = sizeof(az_mcp_tool_v1), .name = "set_options", .description = "Change saved login options.",
          .input_schema = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},"
            "\"autoLogin\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1},"
            "\"remember\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1},"
            "\"delaySeconds\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":600},"
            "\"attempts\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4}},"
            "\"required\":[\"key\"],\"additionalProperties\":false}", .grant = AZ_MCP_SAVED, .invoke = mcp_options },
        { .size = sizeof(az_mcp_tool_v1), .name = "forget", .description = "Ask on the deck before deleting the saved login.",
          .input_schema = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},"
                          "\"required\":[\"key\"],\"additionalProperties\":false}",
          .grant = AZ_MCP_SAVED, .invoke = mcp_forget }
    };
    if (az_mcp_register_tools(C, MOD_ID, tools, sizeof tools / sizeof tools[0], &mcp_provider) != AZ_OK)
        C->log(MOD_ID, "MCP provider startup failed");
}
#endif

static int init(const az_core_v1 *core)
{
    static const az_row_v1 row = {
        .size = sizeof(az_row_v1), .key = "menu", .label = "Beatport Remember", .kind = AZ_ROW_SCREEN, .help = "Saved Beatport login, auto login and forget", .screen = &PAGE
    };
    if (!core || core->abi != AZ_MOD_ABI) return 1;
    C = core;
    if (!core_features_ready()) {
        C->log(MOD_ID, "Beatport Remember requires AZ-MODS 1.2.8 or newer");
        return 2;
    }
    login_service = C->beatport_login_service(AZ_BEATPORT_LOGIN_ABI);
    if (!login_contract_available()) {
        C->log(MOD_ID, "Beatport login service is unavailable on this firmware");
        return 3;
    }
    load_options();
    if (login_service->configure(MOD_ID, &options) != 0) {
        C->log(MOD_ID, "Beatport login service could not be configured");
        return 4;
    }
    if (C->register_screen(MOD_ID, &PAGE) != 0 || C->register_row(MOD_ID, &row) < 0) {
        options.auto_login = options.remember = 0;
        login_service->configure(MOD_ID, &options);
        C->log(MOD_ID, "could not register the Beatport Remember menu page");
        return 5;
    }
    action_error = NULL;
    status_ticks = 0;
    refresh_status();
    C->log(MOD_ID, "loaded (auto=%d remember=%d, login delay=%d s, attempts=%d)",
           options.auto_login, options.remember, options.delay_sec, options.max_attempts);
#ifdef BEATPORT_MCP
    register_mcp();
#endif
    return 0;
}

static void tick(void)
{
#ifdef BEATPORT_MCP
    az_mcp_provider_tick(mcp_provider);
#endif
    /* The overlay runs at up to ~30 Hz. Limit status redraws to <4 per second;
     * no firmware UI access or persistence work runs in this callback. */
    if (++status_ticks < 8) return;
    status_ticks = 0;
    if (refresh_status()) redraw();
}

static const char *const FW_TAGS[] = { "any", NULL };
static const az_mod_v1 MOD = {
    .abi = AZ_MOD_ABI,
    .size = sizeof(az_mod_v1),
    .id = MOD_ID,
    .fw_tags = FW_TAGS,
    .init = init,
    .tick = tick,
    .on_frame = NULL,
};

const az_mod_v1 *az_mod_v1_entry(void) { return &MOD; }
