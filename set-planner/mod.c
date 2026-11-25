/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod.h"
#include <limits.h>

static SpRuntime runtime, settings;
static az_screen_v1 screen;
static az_surface_v1 surface;

double sp_clamp(double value, double low, double high)
{
    if (value < low)
        return low;
    return value > high ? high : value;
}

static bool library_ready(const az_library_v1 *library)
{
    if (!AZ_LIB_HAS(library, playlist_move_status))
        return false;
    return library->lib_playlists && library->lib_tracks && library->track_resolve &&
        library->track_request && library->track_acquire && library->track_release;
}

static bool editing_ready(const az_library_v1 *library)
{
    return library->waveform_request && library->waveform_acquire &&
        library->playlist_order && library->playlist_move;
}

static bool core_ready(const az_core_v1 *core)
{
    if (!AZ_HAS(core, plan_set_resolve))
        return false;
    return core->library_service && core->on_plan_set && core->register_screen && core->thread_start;
}

static bool surface_ready(const az_core_v1 *core)
{
    return core->surface_create && core->surface_place && core->surface_show &&
        core->surface_focus && core->surface_redraw && core->view;
}

static bool ui_ready(const az_core_v1 *core)
{
    if (!core->row_height || !core->text_width)
        return false;
    return core->confirm && core->draw_text && core->fill_rect && core->color &&
        core->request_redraw && core->cfg_get && core->cfg_set;
}

static int setting(const char *name, int value, int first, int last)
{
    return (int)sp_clamp(settings.core->cfg_get(SP_ID, name, value), first, last);
}

static void load_settings(const az_core_v1 *core)
{
    settings.core = core;
    settings.options.curve = setting("curve", SP_PRIMETIME, 0, SP_CURVES - 1);
    settings.options.max_bpm = setting("tempo-limit", 18, 4, 18);
    settings.options.halftime = setting("half-tempo", 1, 0, 1);
    settings.options.time_aware = setting("time-aware", 1, 0, 1);
    settings.energy_choice.mode = setting("energy-mode", SP_PHRASES, SP_FULL, SP_ENERGY_COUNT - 1);
    settings.energy_choice.kind = setting("phrase-type", AZ_PHRASE_CHORUS, AZ_PHRASE_INTRO, AZ_PHRASE_BRIDGE);
    settings.energy_choice.occurrence = setting("occurrence", SP_FIRST, 0, SP_OCCURRENCE_COUNT - 1);
}

void sp_redraw(SpRuntime *state)
{
    if (state->planner_open)
        state->core->surface_redraw(state->surface);
    else if (!state->surface)
        state->core->request_redraw();
}

void sp_close_planner(SpRuntime *state)
{
    state->core->surface_show(state->surface, 0);
    state->planner_open = false;
    if (atomic_load(&state->phase) == SP_WORKING)
        atomic_store(&state->cancel, true);
}

static bool can_open(const SpRuntime *state, const az_plan_set_target_v1 *target)
{
    return target->playlist_id > 0 && target->playlist_id <= INT_MAX &&
        atomic_load(&state->phase) == SP_IDLE && !state->planner_open &&
        !state->confirmation_pending &&
        state->core->view() == AZ_VIEW_BROWSE;
}

static void set_planner_options(SpRuntime *state)
{
    state->energy_choice = settings.energy_choice;
    state->options = settings.options;
    state->options.opener = -1;
    state->options.closer = -1;
    state->options.cancelled = sp_cancelled;
    state->options.user = state;
}

static void open_from_menu(const az_plan_set_target_v1 *target, void *user)
{
    SpRuntime *state = user;
    if (!can_open(state, target))
        return;
    state->core->surface_place(state->surface, (az_rect){0, 70, 1280, 634});
    state->core->surface_show(state->surface, 1);
    if (!state->core->surface_focus(state->surface, 1)) {
        sp_close_planner(state);
        return;
    }
    state->planner_open = true;
    state->launch = *target;
    state->page = SP_NOTICE;
    state->cursor = state->top = 0;
    state->result = AZ_OK;
    state->sync_failed = false;
    set_planner_options(state);
    sp_post_job(state, SP_LOAD);
}

static int register_ui(const az_core_v1 *core)
{
    screen = sp_screen;
    screen.user = &settings;
    if (core->register_screen(SP_ID, &screen) != 0)
        return AZ_FAILED;
    surface = sp_surface;
    surface.user = &runtime;
    runtime.surface = core->surface_create(SP_ID, &surface);
    if (!runtime.surface)
        return AZ_FAILED;
    return core->on_plan_set(SP_ID, open_from_menu, &runtime);
}

static int init(const az_core_v1 *core)
{
    if (!core_ready(core) || !surface_ready(core) || !ui_ready(core))
        return AZ_UNSUPPORTED;
    const az_library_v1 *library = core->library_service(AZ_LIBRARY_ABI);
    if (!library_ready(library) || !editing_ready(library))
        return AZ_UNSUPPORTED;
    runtime.core = core;
    runtime.library = library;
    load_settings(core);
    int status = register_ui(core);
    if (status != AZ_OK)
        return status;
    return core->thread_start(SP_ID, "planner", sp_worker, &runtime);
}

static void tick(void)
{
    if (runtime.planner_open && runtime.core->view() != AZ_VIEW_BROWSE)
        sp_close_planner(&runtime);
    sp_ui_tick(&runtime);
}

AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void)
{
    static const char *const firmware[] = {"any", NULL};
    static const az_mod_v1 mod = {.abi = AZ_MOD_ABI, .size = sizeof mod, .id = SP_ID,
        .fw_tags = firmware, .init = init, .tick = tick};
    return &mod;
}
