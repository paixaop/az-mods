/* SPDX-License-Identifier: GPL-3.0-only */
#include "job.h"
#include "source-icon.h"

#define YT_STORAGE_KEY "storage-mode"

static struct youtube_settings {
    const az_core_v1 *core;
    const az_source_v1 *source;
    enum az_source_storage_mode active;
    int row;
} settings;

static enum az_source_storage_mode mode_for_choice(int choice)
{
    return choice == 1 ? AZ_SOURCE_IMPORT : AZ_SOURCE_STREAMING;
}

static int choice_for_mode(enum az_source_storage_mode mode)
{
    return mode == AZ_SOURCE_IMPORT;
}

static void change_mode(int choice, void *user)
{
    struct youtube_settings *state = user;
    enum az_source_storage_mode mode = mode_for_choice(choice);
    if (mode == state->active)
        return;
    if (state->source->set_storage_mode(YT_MOD_ID, mode) != AZ_OK) {
        /* Mod Menu saves a row before notification. Revert rejected changes. */
        (void)state->core->row_set(state->row, choice_for_mode(state->active));
        return;
    }
    state->active = mode;
    state->core->cfg_set(YT_MOD_ID, YT_STORAGE_KEY, choice);
}

static int register_settings(const az_core_v1 *api, const az_source_v1 *source)
{
    static const char *const choices[] = {"Streaming", "Import"};
    int saved = api->cfg_get(YT_MOD_ID, YT_STORAGE_KEY, 0);
    settings = (struct youtube_settings){.core = api, .source = source,
                                         .active = mode_for_choice(saved)};
    int choice = choice_for_mode(settings.active);
    const az_row_v1 row = {.size = sizeof(row), .key = YT_STORAGE_KEY,
        .kind = AZ_ROW_CHOICE, .label = "YouTube storage", .choices = choices,
        .n_choices = 2, .default_choice = choice, .on_change = change_mode, .user = &settings,
        .help = "Streaming keeps tracks for playback. Import saves tracks in the USB library."};
    settings.row = api->register_row(YT_MOD_ID, &row);
    if (settings.row < 0)
        return AZ_FAILED;
    return api->row_set(settings.row, choice) == 0 ? AZ_OK : AZ_FAILED;
}

static int setting_services(const az_core_v1 *api)
{
    return api->cfg_get && api->cfg_set && api->register_row && api->row_set;
}

static int init(const az_core_v1 *api)
{
    if (!AZ_HAS(api, source_service) || !yt_job_services(api))
        return AZ_UNSUPPORTED;
    const az_source_v1 *source = api->source_service(AZ_SOURCE_ABI);
    if (!AZ_SOURCE_HAS(source, register_provider) || !AZ_SOURCE_HAS(source, set_storage_mode))
        return AZ_UNSUPPORTED;
    if (!setting_services(api))
        return AZ_UNSUPPORTED;
    int status = register_settings(api, source);
    if (status != AZ_OK)
        return status;
    const az_source_provider_v1 provider = {
        .size = sizeof(provider),
        .name = "YouTube",
        .icon_png = yt_source_icon,
        .icon_bytes = sizeof(yt_source_icon),
        .storage_mode = settings.active,
        .user = (void *)api,
        .list_playlists = yt_list_playlists,
        .playlist_tracks = yt_playlist_tracks,
        .search = yt_search_tracks,
        .prepare_track = yt_prepare_track,
    };
    return source->register_provider(YT_MOD_ID, &provider);
}

static const char *const firmware[] = {"ep147-9e9454e357389ece", NULL};
static const az_mod_v1 mod = {
    .abi = AZ_MOD_ABI,
    .size = sizeof(mod),
    .id = YT_MOD_ID,
    .fw_tags = firmware,
    .init = init,
};

AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void)
{
    return &mod;
}
