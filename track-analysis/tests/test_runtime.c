/* SPDX-License-Identifier: GPL-3.0-only */
#include "../track-analysis.c"
#include <assert.h>
static int enabled = 1, banners;
static int row_value(int row) { assert(row == 1); return enabled; }
static void banner(const char *id, const char *text, int ms) {
    assert(!strcmp(id, ID) && text && ms > 0); banners++;
}
int main(void) {
    az_core_v1 core = {.abi = AZ_MOD_ABI, .size = sizeof core, .row_value = row_value, .banner = banner};
    C = &core; enabled_row = 1;
    az_status_load_v1 load = {.header = {.size = sizeof load, .version = AZ_DATA_VERSION,
        .valid = (UINT64_C(1) << AZ_STATUS_LOAD_STATE) | (UINT64_C(1) << AZ_STATUS_LOAD_TRACK)},
        .state = AZ_STATUS_LOAD_LOADED, .track = {.size = sizeof(az_track_ref), .version = AZ_DATA_VERSION,
            .source_id = 1, .source_generation = 2, .library_id = 3, .track_id = 4},
        .state_info = {.info = {.availability = AZ_AVAIL_READY, .provenance = AZ_PROV_NATIVE}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = 42},
        .track_info = {.info = {.availability = AZ_AVAIL_READY}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = 42}};
    az_deck_status_view_v1 view = {.size = sizeof view, .version = AZ_DATA_VERSION, .load = &load};
    az_deck_status_event_v1 event = {.size = sizeof event, .version = AZ_DATA_VERSION, .deck = 0,
        .load_epoch = 42, .view = &view};
    deck_status(&event, NULL); tick();
    assert(atomic_load(&work[0].phase) == 1 && work[0].track.track_id == 4 && work[0].epoch == 42);
    deck_status(&event, NULL); tick();
    assert(atomic_load(&work[0].phase) == 1 && desired[0].attempted == 42);
    atomic_store(&work[0].phase, 2);
    event.load_epoch = 43; load.track_info.scope_id = load.state_info.scope_id = 43;
    load.track.track_id = 5; deck_status(&event, NULL);
    assert(atomic_load(&work[0].cancelled));
    work[0].result = AZ_OK; atomic_store(&work[0].phase, 3); tick();
    assert(!banners && atomic_load(&work[0].phase) == 1 && work[0].epoch == 43);
    enabled = 0; tick();
    assert(atomic_load(&work[0].cancelled) && !desired[0].attempted);
    work[0].result = AZ_STALE; atomic_store(&work[0].phase, 3); tick();
    enabled = 1; tick(); assert(atomic_load(&work[0].phase) == 1);
    atomic_store(&work[0].phase, 3); work[0].result = AZ_OK; tick();
    assert(banners == 1 && atomic_load(&work[0].phase) == 0);
    desired[0].attempted = 0; tick();
    work[0].result = AZ_STALE; atomic_store(&work[0].phase, 3); tick();
    assert(atomic_load(&work[0].phase) == 1); /* cue/file edits retry while the same load remains */
    atomic_store(&work[0].cancelled, 1);
    atomic_store(&work[0].phase, 3); tick();
    assert(atomic_load(&work[0].phase) == 0);
    /* Unqualified FILE identity and unknown load states never queue analysis. */
    event.load_epoch = 44; load.header.valid = UINT64_C(1) << AZ_STATUS_LOAD_STATE;
    deck_status(&event, NULL); tick(); assert(!desired[0].loaded && !atomic_load(&work[0].phase));
    load.header.valid |= UINT64_C(1) << AZ_STATUS_LOAD_TRACK;
    load.track_info.scope = AZ_STATUS_UNIT;
    load.state_info.scope_id = event.load_epoch;
    deck_status(&event, NULL); tick(); assert(!desired[0].loaded && !atomic_load(&work[0].phase));
    assert(init(NULL) == -1);
    puts("Track Analysis queue, load cancellation, enable retry and stale completion tests passed");
}
