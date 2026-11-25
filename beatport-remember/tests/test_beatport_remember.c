/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the shipped callbacks against the public service contract. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../beatport-remember.c"

struct az_draw { int unused; };
static az_beatport_login_status published;
static az_beatport_login_options configured;
static int configure_calls, configure_result, status_result, forget_calls, forget_result;
static int row_result, registered, cfg_writes, redraw_count;
static int cfg_auto = 1, cfg_remember = 1, cfg_delay = 45, cfg_attempts = 1;
static const az_screen_v1 *screen;
static char last_log[256], drawn[1024];
static az_beatport_login_v1 service;
static int service_available = 1;

static void log_line(const char *id, const char *fmt, ...)
{
    va_list ap;
    assert(!strcmp(id, MOD_ID));
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
}
static int configure(const char *id, const az_beatport_login_options *o)
{
    assert(!strcmp(id, MOD_ID));
    assert(o->size == sizeof(*o));
    configure_calls++;
    if (configure_result) return configure_result;
    configured = *o;
    return 0;
}
static int status(const char *id, az_beatport_login_status *s)
{
    assert(!strcmp(id, MOD_ID));
    assert(s->size == sizeof(*s));
    if (status_result) return status_result;
    *s = published;
    return 0;
}
static int forget(const char *id)
{
    assert(!strcmp(id, MOD_ID));
    forget_calls++;
    if (forget_result) return forget_result;
    published.save_pending = 1;
    return 0;
}
static const az_beatport_login_v1 *get_service(uint32_t abi)
{
    assert(abi == AZ_BEATPORT_LOGIN_ABI);
    return service_available ? &service : NULL;
}
static int get_cfg(const char *id, const char *key, int def)
{
    assert(!strcmp(id, MOD_ID));
    if (!strcmp(key, "auto-login")) return cfg_auto;
    if (!strcmp(key, "remember")) return cfg_remember;
    if (!strcmp(key, "delay-sec")) return cfg_delay;
    if (!strcmp(key, "attempts")) return cfg_attempts;
    return def;
}
static void set_cfg(const char *id, const char *key, int value)
{
    assert(!strcmp(id, MOD_ID));
    if (!strcmp(key, "auto-login")) cfg_auto = value;
    else if (!strcmp(key, "remember")) cfg_remember = value;
    else if (!strcmp(key, "delay-sec")) cfg_delay = value;
    else if (!strcmp(key, "attempts")) cfg_attempts = value;
    else assert(!"unexpected setting write");
    cfg_writes++;
}
static int register_row(const char *id, const az_row_v1 *row)
{
    assert(!strcmp(id, MOD_ID));
    assert(row->kind == AZ_ROW_SCREEN && row->screen);
    registered++;
    screen = row->screen;
    return row_result;
}
static int row_height(az_draw *d) { (void)d; return 40; }
static int register_screen(const char *id, const az_screen_v1 *page)
{
    assert(!strcmp(id, MOD_ID) && page == &PAGE && !page->flags);
    screen = page;
    return 0;
}
static int text_width(const char *s, int px) { return (int)strlen(s) * px / 2; }
static uint32_t color(int role) { return (uint32_t)role; }
static void draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t rgb)
{
    (void)d; (void)x; (void)y; (void)px; (void)rgb;
    assert(strlen(drawn) + strlen(s) + 2 < sizeof(drawn));
    strcat(drawn, s); strcat(drawn, "\n");
}
static void draw_header(az_draw *d, const char *s) { (void)d; (void)s; }
static void draw_checkbox(az_draw *d, int x, int y, int w, int checked, int selected)
{ (void)d; (void)x; (void)y; (void)w; (void)checked; (void)selected; }
static void fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t rgb)
{ (void)d; (void)x; (void)y; (void)w; (void)h; (void)rgb; }
static void request_redraw(void) { redraw_count++; }
static void paint(void)
{
    static az_draw d;
    drawn[0] = 0;
    screen->on_draw(&d, (az_rect){0,0,1000,420}, screen->user);
}
static void tap(az_rect r)
{
    assert(r.w > 0 && r.h > 0);
    az_event e = {.kind=AZ_EV_TAP, .x=r.x + r.w/2, .y=r.y + r.h/2};
    assert(screen->on_input(&e, screen->user));
}
static void step_status(const az_mod_v1 *mod)
{
    for (int i = 0; i < 8; i++) mod->tick();
}

int main(void)
{
    service = (az_beatport_login_v1){ .abi=AZ_BEATPORT_LOGIN_ABI, .size=sizeof(service),
        .configure=configure, .status=status, .forget=forget };
    published.size = sizeof(published);
    az_core_v1 core = {.abi=AZ_MOD_ABI, .size=sizeof(core), .log=log_line,
        .cfg_get=get_cfg, .cfg_set=set_cfg, .register_row=register_row, .register_screen=register_screen,
        .beatport_login_service=get_service, .request_redraw=request_redraw,
        .row_height=row_height, .text_width=text_width, .color=color,
        .draw_text=draw_text, .draw_header=draw_header, .draw_checkbox=draw_checkbox,
        .fill_rect=fill_rect};
    const az_mod_v1 *mod = az_mod_v1_entry();
    assert(!strcmp(mod->fw_tags[0], "any") && !mod->fw_tags[1]);
    assert(mod->on_frame == NULL);
    assert(mod->init(NULL) != 0);
    core.size = offsetof(az_core_v1, beatport_login_service);
    assert(mod->init(&core) != 0 && configure_calls == 0 && registered == 0);
    assert(strstr(last_log, "1.2.8"));
    core.size = sizeof(core);
    service_available = 0;
    assert(mod->init(&core) != 0 && configure_calls == 0);
    assert(strstr(last_log, "unavailable"));
    service_available = 1;
    service.size = offsetof(az_beatport_login_v1, forget);
    assert(mod->init(&core) != 0 && configure_calls == 0);
    service.size = sizeof(service);
    service.abi++;
    assert(mod->init(&core) != 0 && configure_calls == 0);
    service.abi = AZ_BEATPORT_LOGIN_ABI;
    service.forget = NULL;
    assert(mod->init(&core) != 0 && configure_calls == 0);
    service.forget = forget;
    configure_result = -1;
    assert(mod->init(&core) != 0 && registered == 0);
    configure_result = 0;
    row_result = -1;
    assert(mod->init(&core) != 0);
    assert(!configured.auto_login && !configured.remember); /* inactive without a menu */
    row_result = 0;

    cfg_delay = -3; cfg_attempts = 17;
    assert(mod->init(&core) == 0);
    assert(configured.auto_login && configured.remember);
    assert(configured.delay_sec == 0 && configured.max_attempts == 4);
    cfg_delay = 999; cfg_attempts = 0;
    assert(mod->init(&core) == 0);
    assert(configured.delay_sec == 600 && configured.max_attempts == 1);
    cfg_delay = 45; cfg_attempts = 1;
    assert(mod->init(&core) == 0);
    assert(configured.delay_sec == 45 && configured.max_attempts == 1);
    /* Future cores may append fields without changing ABI v1. The same mod
     * binary consumes only its known prefix, independent of the core version. */
    core.size = sizeof(core) + 64;
    service.size = sizeof(service) + 64;
    assert(mod->init(&core) == 0);
    assert(configured.delay_sec == 45 && configured.max_attempts == 1);
    core.size = sizeof(core);
    service.size = sizeof(service);
    screen->on_enter(screen->user); paint();
    assert(strstr(drawn, "No saved login"));
    assert(g_forget_btn.w == 0);

    configure_result = -1;
    tap(g_auto_row); paint();
    assert(cfg_auto == 1 && cfg_writes == 0 && configured.auto_login == 1);
    assert(strstr(drawn, "Could not change login settings"));
    configure_result = 0;
    tap(g_auto_row); paint();
    assert(cfg_auto == 0 && cfg_writes == 1 && configured.auto_login == 0);
    assert(!strstr(drawn, "Could not change"));
    tap(g_rem_row);
    assert(cfg_remember == 0 && cfg_writes == 2 && configured.remember == 0);

    published.have_credentials = 1;
    strcpy(published.username, "dj@example.test");
    int before = redraw_count;
    for (int i = 0; i < 7; i++) mod->tick();
    assert(redraw_count == before);
    mod->tick(); assert(redraw_count == before + 1);
    paint(); assert(strstr(drawn, "Saved login: dj@example.test"));
    before = redraw_count;
    step_status(mod); assert(redraw_count == before);
    forget_result = -1;
    tap(g_forget_btn); paint();
    assert(forget_calls == 1 && strstr(drawn, "Could not forget"));
    forget_result = 0;
    tap(g_forget_btn); paint();
    assert(forget_calls == 2 && strstr(drawn, "Updating saved login"));
    assert(!strstr(drawn, "Saved login: dj@example.test"));
    /* Removal has not completed just because the request was accepted. */
    published.save_pending = 0; published.storage_error = 1;
    step_status(mod); paint();
    assert(strstr(drawn, "Could not update saved login"));
    assert(g_forget_btn.w > 0);
    tap(g_forget_btn);
    published.have_credentials = published.save_pending = published.storage_error = 0;
    published.username[0] = 0;
    step_status(mod); paint();
    assert(strstr(drawn, "No saved login") && !g_forget_btn.w);

    status_result = -1;
    step_status(mod); paint();
    assert(strstr(drawn, "status unavailable") && !g_forget_btn.w);
    status_result = 0;
    published.have_credentials = 1;
    memset(published.username, 'x', sizeof(published.username));
    step_status(mod); paint(); /* defensive termination of service text */
    assert(strstr(drawn, "Saved login:"));
    az_event back = {.kind=AZ_EV_BACK};
    assert(!screen->on_input(&back, screen->user));
    assert(!screen->on_input(NULL, screen->user));
    puts("Beatport Remember public service and page tests passed");
    return 0;
}
