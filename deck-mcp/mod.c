/* SPDX-License-Identifier: GPL-3.0-only */
#include "deck-mcp.h"
#include "providers.h"

#include <stdio.h>
#include <string.h>

static struct mcp_runtime runtime = { .lock = PTHREAD_MUTEX_INITIALIZER };

static bool supports_lifecycle(const az_core_v1 *core)
{
    return AZ_HAS(core, thread_start) && AZ_HAS(core, observation_service) &&
           AZ_HAS(core, data_dir) && AZ_HAS(core, log);
}

static bool subscribe(void)
{
    const az_deck_watch_v1 watch = {
        .size = sizeof watch,
        .version = AZ_DATA_VERSION,
        .deck_mask = AZ_STATUS_ALL_DECKS,
        .position_hz = 5,
        .topics = mcp_watch_topics()
    };
    return runtime.observation->on_deck_status(MCP_ID, &watch, mcp_capture, &runtime,
                                               &runtime.subscription) == AZ_OK;
}

static void unsubscribe(void)
{
    if (runtime.subscription)
        runtime.observation->deck_status_release(MCP_ID, runtime.subscription);
    runtime.subscription = 0;
}

static void reset_runtime(const az_core_v1 *core)
{
    runtime.core = core;
    runtime.analysis = AZ_HAS(core, analysis_service) ? core->analysis_service(AZ_ANALYSIS_ABI) : NULL;
    for (size_t i = 0; i < MCP_ANALYSIS_KEYS; ++i) {
        cmcp_json_free(runtime.analysis_keys[i].args);
        runtime.analysis_keys[i] = (struct mcp_analysis_record){0};
    }
    runtime.library = AZ_HAS(core, library_service) ? core->library_service(AZ_LIBRARY_ABI) : NULL;
    runtime.pending = 0;
    runtime.settings = (struct mcp_settings){0};
    runtime.panel = (struct mcp_panel){0};
    memset(runtime.ui, 0, sizeof runtime.ui);
    memset(runtime.published, 0, sizeof runtime.published);
    mcp_live_reset();
    snprintf(runtime.firmware, sizeof runtime.firmware, "%s",
             AZ_HAS(core, fw_tag) ? core->fw_tag() : "unknown");
    atomic_store(&runtime.stopping, false);
    atomic_store(&runtime.working, true);
}

static int init(const az_core_v1 *core)
{
    if (!supports_lifecycle(core) || atomic_load(&runtime.working) || atomic_load(&runtime.library_io.working))
        return -1;
    runtime.observation = core->observation_service(AZ_OBSERVATION_ABI);
    if (!AZ_OBSERVATION_HAS(runtime.observation, on_deck_status) ||
        !AZ_OBSERVATION_HAS(runtime.observation, deck_status_release))
        return -2;
    reset_runtime(core);
    mcp_commands_init(&runtime);
    mcp_provider_controls_init(&runtime);
    mcp_settings_tick(&runtime);
    mcp_token_screen_init(&runtime);
    if (subscribe()) {
        mcp_start_library_io(&runtime);
        if (core->thread_start(MCP_ID, "deck-mcp", mcp_serve, &runtime) == 0)
            return 0;
        atomic_store(&runtime.stopping, true);
    }
    unsubscribe();
    atomic_store(&runtime.working, false);
    return -3;
}

static void tick(void)
{
    mcp_publish(&runtime);
    mcp_commands_tick(&runtime);
    mcp_settings_tick(&runtime);
    mcp_token_screen_tick(&runtime);
}

static void stop(void)
{
    atomic_store(&runtime.stopping, true);
    atomic_store(&runtime.commands.grant, 0);
    atomic_store(&runtime.saved_edits, 0);
    unsubscribe();
    /* The SDK waits for its worker after stop returns. All worker storage is static. */
}

static const char *const firmware[] = { "any", NULL };
static const az_mod_v1 mod = {
    .abi = AZ_MOD_ABI,
    .size = sizeof mod,
    .id = MCP_ID,
    .fw_tags = firmware,
    .init = init,
    .tick = tick,
    .stop = stop
};

AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void)
{
    return &mod;
}
