/* SPDX-License-Identifier: GPL-3.0-only */
#include "../cues.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static cues_config config;
static int event_kind = AZ_STATUS_CHANGE;
static int qualified_track = 1, load_metadata;
static int maps_mode;
static const az_screen_v1 screen = { .size = sizeof screen };
static az_command_request submitted[16];
static int submissions, acknowledgements, cancellations, threads, refusals;
static int acknowledge_busy, phrase_available = 1, confirmations,
                             planned_count = 2;
static void (*confirm_callback)(int, void *);
static void *confirm_user;
static az_surface_v1 surface_descriptors[2];
static unsigned surface_count, painted_labels;
static int surface_focus_on;
static az_rect paint_area;
static int ask(const char *id, const char *title, const char *text,
               const char *yes, void (*fn)(int, void *), void *u)
{
    (void)id;
    (void)title;
    assert(strstr(text, "replace all") && !strcmp(yes, "Replace"));
    ++confirmations;
    confirm_callback = fn;
    confirm_user = u;
    return 0;
}
static az_surface *create_surface(const char *id, const az_surface_v1 *s)
{
    static uintptr_t serial;
    (void)id;
    assert(s->views == AZ_VIEW_BIT(AZ_VIEW_DECK));
    assert(surface_count < 2);
    surface_descriptors[surface_count++] = *s;
    return (az_surface *)++serial;
}
static void place_surface(az_surface *s, az_rect r)
{
    assert(s && r.w >= 44 && r.h >= 44 && r.x + r.w <= 1280 &&
           r.y + r.h <= 800);
}
static void redraw_surface(az_surface *s) { assert(s); }
static void show_surface(az_surface *s, int on)
{
    (void)on;
    assert(s);
}
static int focus_surface(az_surface *surface, int on)
{
    assert(surface == deck_picker);
    surface_focus_on = on;
    return 1;
}
static uint32_t draw_color(int role) { return (uint32_t)role; }
static void draw_fill(az_draw *draw, int x, int y, int w, int h, uint32_t color)
{
    (void)draw;
    (void)color;
    assert(x >= paint_area.x && y >= paint_area.y && w > 0 && h > 0);
    assert(x + w <= paint_area.x + paint_area.w && y + h <= paint_area.y + paint_area.h);
}
static void draw_border(az_draw *draw, int x, int y, int w, int h, int thickness, uint32_t color)
{
    assert(thickness == 1 || thickness == 2);
    draw_fill(draw, x, y, w, h, color);
}
static void draw_label(az_draw *draw, int x, int y, const char *text, int px, uint32_t color)
{
    (void)draw;
    (void)color;
    assert(x >= paint_area.x && y >= paint_area.y && px == 14 && text[0]);
    assert(x < paint_area.x + paint_area.w && y + px <= paint_area.y + paint_area.h);
    ++painted_labels;
}
static char shown[128];
static az_deck_snapshot live;
static az_track_ref status_track;
static unsigned load_provenance = AZ_PROV_NATIVE;
static az_native_cue_snapshot bank;
static az_deck_status_fn listener;
static az_command_completion_fn callback;
static void *callback_user;
const az_screen_v1 *cues_settings_init(const az_core_v1 *core,
                                       void (*changed)(void))
{
    (void)core;
    (void)changed;
    return &screen;
}
const cues_config *cues_settings_config(void) { return &config; }
void cues_settings_status(const char *text)
{
    snprintf(shown, sizeof shown, "%s", text);
}
void cues_settings_tick(void) {}
int cues_build_plan(const cues_config *c, const az_track_snapshot *track,
                    const az_native_cue_snapshot *existing, uint32_t rate,
                    cues_plan *out)
{
    assert(c->enabled);
    if (qualified_track || load_metadata) {
        assert(track->metadata.valid & AZ_META_ARTIST);
        assert(!strcmp(track->metadata.artist, "Test artist"));
    } else {
        assert(!(track->metadata.valid & (AZ_META_ARTIST | AZ_META_TITLE)));
    }
    assert(rate == 48000 && existing->revision);
    *out = (cues_plan){ .count = planned_count };
    out->points[0] = (cues_point){ .classification = AZ_CUE_HOT,
                                   .slot = 2,
                                   .type = AZ_CUE_POINT,
                                   .start_s = 0,
                                   .rgb = 0x00e300 };
    out->points[1] = (cues_point){ .classification = AZ_CUE_MEMORY,
                                   .type = AZ_CUE_LOOP,
                                   .start_s = 4,
                                   .end_s = 8 };
    strcpy(out->points[1].comment, "Drop");
    return AZ_OK;
}
static int get_deck(int deck, az_deck_snapshot *out)
{
    if (deck)
        return AZ_EMPTY;
    *out = live;
    return AZ_OK;
}
static int get_cues(int deck, az_native_cue_snapshot *out)
{
    if (deck)
        return AZ_EMPTY;
    *out = bank;
    return AZ_OK;
}
static int capabilities(az_data_capabilities *out)
{
    out->version = AZ_DATA_VERSION;
    out->commands = WRITE_COMMANDS;
    return AZ_OK;
}
static int watch(const char *id, const az_deck_watch_v1 *w,
                 az_deck_status_fn fn, void *u, az_deck_subscription *s)
{
    (void)u;
    assert(!strcmp(id, CUES_ID) && w->deck_mask == 15);
    listener = fn;
    *s = 1;
    return AZ_OK;
}
static int release_watch(const char *id, az_deck_subscription s)
{
    (void)id;
    (void)s;
    return AZ_OK;
}
static int submit(const char *id, const az_command_request *r,
                  az_command_completion_fn fn, void *u,
                  az_command_result *out)
{
    (void)out;
    assert(!strcmp(id, CUES_ID));
    assert(r->operation == AZ_CMD_CUE_POINT_SET ||
           r->operation == AZ_CMD_CUE_LOOP_SET ||
           r->operation == AZ_CMD_HOT_CLEAR ||
           r->operation == AZ_CMD_MEMORY_DELETE);
    assert(r->flags & AZ_COMMAND_PERSIST);
    if (refusals) {
        --refusals;
        return AZ_BUSY;
    }
    assert(submissions < 16);
    submitted[submissions++] = *r;
    callback = fn;
    callback_user = u;
    return AZ_OK;
}
static int acknowledge(const char *id, uint64_t request)
{
    (void)id;
    assert(request);
    ++acknowledgements;
    if (acknowledge_busy) {
        acknowledge_busy = 0;
        return AZ_BUSY;
    }
    return AZ_OK;
}
static int cancel(const char *id, uint64_t request, az_command_result *out)
{
    (void)id;
    (void)out;
    assert(request);
    ++cancellations;
    return AZ_PENDING;
}
static int start_thread(const char *id, const char *name, void *(*fn)(void *),
                        void *arg)
{
    (void)id;
    (void)name;
    (void)fn;
    (void)arg;
    ++threads;
    return 0;
}
static int register_screen(const char *id, const az_screen_v1 *s)
{
    (void)id;
    assert(s == &screen);
    return 0;
}
static int get_target(int deck, uint64_t epoch, az_command_target_v1 *out)
{
    az_deck_snapshot snapshot = { .size = sizeof snapshot };
    az_native_cue_snapshot cues = { .size = sizeof cues };
    uint64_t load;
    if (get_deck(deck, &snapshot) != AZ_OK || !(snapshot.valid & AZ_LIVE_LOADED) || snapshot.loaded != 1)
        return AZ_PENDING;
    if (!az_read_native_load_target(&snapshot, (uint32_t)deck, now_ns(), &load) || load != epoch)
        return AZ_STALE;
    if (get_cues(deck, &cues) != AZ_OK || cues.version != AZ_DATA_VERSION || cues.deck != (uint32_t)deck ||
        cues.native_load_generation != load || !cues.revision || !cues.source_rate_hz ||
        cues.source_rate_hz >= 0x7fffffffu || cues.count > AZ_NATIVE_CUE_LIMIT) return AZ_STALE;
    *out = (az_command_target_v1){ .size = sizeof *out, .version = AZ_DATA_VERSION,
                                 .commands = WRITE_COMMANDS, .deck = snapshot, .cues = cues };
    return AZ_OK;
}
static az_library_v1 library = { .abi = AZ_LIBRARY_ABI,
                                 .size = sizeof library,







                                 };
static az_observation_v1 library_observation = { .abi = AZ_OBSERVATION_ABI, .size = sizeof library_observation, .command_target = get_target, .data_capabilities = capabilities, .deck_snapshot = get_deck, .native_cues = get_cues, .on_deck_status = watch, .deck_status_release = release_watch };
static az_commands_v1 library_commands = { .abi = AZ_COMMANDS_ABI, .size = sizeof library_commands, .command_submit_async = submit, .command_acknowledge = acknowledge, .command_cancel = cancel };
static const az_library_v1 *service(uint32_t abi)
{
    assert(abi == AZ_LIBRARY_ABI);
    return &library;
}
static const az_observation_v1 *service_observation(uint32_t abi)
{
    assert(abi == AZ_OBSERVATION_ABI);
    return &library_observation;
}

static const az_commands_v1 *service_commands(uint32_t abi)
{
    assert(abi == AZ_COMMANDS_ABI);
    return &library_commands;
}

static az_core_v1 core = { .abi = AZ_MOD_ABI,
                           .size = sizeof core,
                           .library_service = service, .observation_service = service_observation, .command_service = service_commands,
                           .thread_start = start_thread,
                           .register_screen = register_screen,
                           .confirm = ask,
                           .surface_create = create_surface,
                           .surface_place = place_surface,
                           .surface_show = show_surface,
                           .surface_redraw = redraw_surface,
                           .surface_focus = focus_surface,
                           .fill_rect = draw_fill,
                           .draw_rect = draw_border,
                           .draw_text = draw_label,
                           .color = draw_color };
static az_status_field_info_v1 receipt(uint64_t epoch, unsigned scope,
                                       unsigned count)
{
    return (az_status_field_info_v1){ .scope = scope,
                                      .scope_id = epoch,
                                      .count = count,
                                      .provider_id = 99,
                                      .info = { .availability = AZ_AVAIL_READY,
                                                .provenance = AZ_PROV_NATIVE,
                                                .revision = 1 } };
}
static void deliver(uint32_t rate, uint64_t epoch, int loaded)
{
    az_status_load_v1 load = {
        .header = { .size = sizeof load,
                    .version = AZ_DATA_VERSION,
                    .supported = 15,
                    .valid = 15 },
        .state = loaded ? AZ_STATUS_LOAD_LOADED : AZ_STATUS_LOAD_EMPTY,
        .track = status_track,
        .state_info = receipt(88, AZ_STATUS_UNIT, 1),
        .track_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 1)
    };
    load.state_info.info.provenance = load_provenance;
    if (!qualified_track) {
        load.header.valid &= ~az_status_field_bit(AZ_STATUS_LOAD_TRACK);
        load.track_info.info.availability = AZ_AVAIL_UNSUPPORTED;
        /* Keep a stale nonzero payload to prove validity is respected. */
    }
    az_status_metadata_v1 meta = {
        .header = { .size = sizeof meta,
                    .version = AZ_DATA_VERSION,
                    .supported = 127,
                    .valid = 127 },
        .content_id = status_track.track_id,
        .title = "Test track",
        .artist = "Test artist",
        .duration_s = 120,
        .content_id_info = receipt(88, AZ_STATUS_UNIT, 1),
        .title_info = receipt(88, AZ_STATUS_UNIT, 10),
        .artist_info = receipt(88, AZ_STATUS_UNIT, 11),
        .duration_s_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 1)
    };
    if (load_metadata) {
        meta.title_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 10);
        meta.artist_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 11);
    }
    az_status_position_v1 pos = { .header = { .size = sizeof pos,
                                              .version = AZ_DATA_VERSION,
                                              .supported = 1,
                                              .valid = 1 },
                                  .head = { .rate_hz = rate },
                                  .head_info =
                                      receipt(epoch, AZ_STATUS_LOAD_SCOPE, 1) };
    az_status_phrases_v1 phrases = {
        .header = { .size = sizeof phrases,
                    .version = AZ_DATA_VERSION,
                    .supported = 1 },
        .current_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 0)
    };
    phrases.current_info.info.availability =
        phrase_available ? AZ_AVAIL_EMPTY : AZ_AVAIL_LOADING;
    az_cue_entry hot[AZ_NATIVE_CUE_LIMIT], memory[AZ_NATIVE_CUE_LIMIT];
    unsigned nh = 0, nm = 0;
    for (unsigned i = 0; i < bank.count; i++) {
        if (bank.entries[i].classification == AZ_CUE_HOT)
            hot[nh++] = bank.entries[i];
        else
            memory[nm++] = bank.entries[i];
    }
    az_status_cues_v1 current = {
        .header = { .size = sizeof current,
                    .version = AZ_DATA_VERSION,
                    .supported = 31,
                    .valid = 31 },
        .live_hot = hot,
        .live_memory = memory,
        .live_hot_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, nh),
        .live_memory_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, nm)
    };
    if (!nh)
        current.live_hot_info.info.availability = AZ_AVAIL_EMPTY;
    if (!nm)
        current.live_memory_info.info.availability = AZ_AVAIL_EMPTY;
    az_deck_status_view_v1 view = { .size = sizeof view,
                                    .version = AZ_DATA_VERSION,
                                    .load = &load,
                                    .metadata = &meta,
                                    .position = &pos,
                                    .phrases = &phrases,
                                    .cues = &current };
    az_beat beats[] = { { .time_s = 0, .bpm = 120, .beat_in_bar = 1 },
                        { .time_s = .5, .bpm = 120, .beat_in_bar = 2 } };
    az_phrase phrase = { .start_beat = 0, .end_beat = 2, .raw_kind = 17 };
    az_status_grid_v1 grid = { .header = { .size = sizeof grid, .version = AZ_DATA_VERSION, .supported = 1, .valid = 1 }, .current = beats, .current_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 2) };
    az_status_key_v1 key = { .header = { .size = sizeof key, .version = AZ_DATA_VERSION, .supported = 1, .valid = 1 }, .original = "Am", .original_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 2) };
    az_status_tempo_v1 tempo = { .header = { .size = sizeof tempo, .version = AZ_DATA_VERSION, .supported = 1, .valid = 1 }, .original_bpm = 120, .original_bpm_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 1) };
    if (maps_mode) {
        grid.current_info.info.revision = maps_mode == 5 ? 8 : 7;
        phrases.header.valid = phrases.header.supported =
            az_status_field_bit(AZ_STATUS_PHRASES_CURRENT) | az_status_field_bit(AZ_STATUS_PHRASES_CURRENT_GRID);
        phrases.current = &phrase;
        phrases.current_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 1);
        phrases.current_info.info.revision = 9;
        phrases.current_grid_info = receipt(epoch, AZ_STATUS_LOAD_SCOPE, 1);
        phrases.current_grid = (az_status_grid_ref_v1){ .load_epoch = epoch, .provider_id = 99, .revision = grid.current_info.info.revision, .provenance = AZ_PROV_NATIVE };
        if (maps_mode == 2) ++phrases.current_grid.revision;
        if (maps_mode == 3) {
            phrases.current_info.count = 0;
            phrases.current_info.info.availability = AZ_AVAIL_EMPTY;
        }
        if (maps_mode == 4) grid.current_info.info.availability = AZ_AVAIL_STALE;
        view.grid = &grid;
        view.key = &key;
        view.tempo = &tempo;
    }
    az_deck_status_event_v1 event = { .size = sizeof event,
                                      .version = AZ_DATA_VERSION,
                                      .kind = event_kind,
                                      .deck = 0,
                                      .load_epoch = epoch,
                                      .topics = TOPICS,
                                      .view = &view };
    listener(&event, NULL);
}
static void step(void)
{
    next_check = 0;
    tick();
}
static az_command_completion terminal(unsigned outcome, uint64_t revision)
{
    return (az_command_completion){
        .size = sizeof(az_command_completion),
        .version = AZ_DATA_VERSION,
        .kind = AZ_COMMAND_COMPLETION_TERMINAL,
        .settled = 1,
        .result = { .size = sizeof(az_command_result),
                    .version = AZ_DATA_VERSION,
                    .request_id = submitted[submissions - 1].request_id,
                    .outcome = outcome,
                    .reason = outcome == AZ_COMMAND_APPLIED ? AZ_OK : AZ_STALE,
                    .revision = revision,
                    .observed_ns = now_ns() }
    };
}
static void release(az_command_completion c)
{
    c.kind = AZ_COMMAND_COMPLETION_RELEASED;
    callback(&c, callback_user);
}
static void picker_interaction(void)
{
    assert(surface_count == 2);
    paint_area = (az_rect){ 866, 4, 76, 44 };
    surface_descriptors[0].on_paint(NULL, paint_area, NULL);
    paint_area = (az_rect){ 866, 56, 244, 196 };
    surface_descriptors[1].on_paint(NULL, paint_area, NULL);
    assert(painted_labels == 5);
    az_event event = { .kind = AZ_EV_BACK };
    assert(!surface_descriptors[0].on_input(&event, NULL));
    event.kind = AZ_EV_CLICK;
    assert(surface_descriptors[0].on_input(&event, NULL) && picker_open && surface_focus_on);
    event.kind = AZ_EV_TAP;
    event.y = 500;
    assert(!surface_descriptors[1].on_input(&event, NULL) && picker_open);
    event.kind = AZ_EV_KNOB;
    event.dir = -1;
    assert(surface_descriptors[1].on_input(&event, NULL) && picker_selected == 3);
    event.kind = AZ_EV_CLICK;
    assert(surface_descriptors[1].on_input(&event, NULL));
    assert(!picker_open && !surface_focus_on && strstr(shown, "load a track first"));
    assert(surface_descriptors[0].on_input(&event, NULL) && picker_open);
    event.kind = AZ_EV_BACK;
    assert(surface_descriptors[1].on_input(&event, NULL) && !picker_open);
    assert(!decks[0].manual_requested);
    event.kind = AZ_EV_TAP;
    assert(surface_descriptors[0].on_input(&event, NULL) && picker_open);
    event.y = 80;
    assert(surface_descriptors[1].on_input(&event, NULL) && !picker_open);
    assert(decks[0].manual_requested && strstr(shown, "waiting for analysis"));
}
static void analysis_receipts(void)
{
    struct deck_state *d = &decks[0];
    maps_mode = 1;
    deliver(44100, 27, 1);
    assert(d->track.beat_count == 2 && d->track.phrase_count == 1);
    assert(d->grid_revision == 7 && d->phrase_revision == 9);
    assert(compare_timestamps(d->beats[1].time_s, .5, 1, 44100, 1) == AZ_TIME_EQUAL);
    assert(d->phrases[0].end_beat == 2 && d->phrases[0].raw_kind == 17);
    assert((d->track.metadata.valid & (AZ_META_KEY | AZ_META_TEMPO)) == (AZ_META_KEY | AZ_META_TEMPO));
    assert(!strcmp(d->track.metadata.key, "Am") && d->track.metadata.original_bpm == 120);
    uint64_t revision = d->revision;
    deliver(44100, 27, 1);
    assert(d->revision == revision);
    maps_mode = 2;
    deliver(44100, 27, 1);
    assert(d->track.beat_count == 2 && !d->track.phrase_count && d->track.phrases_info.availability == AZ_AVAIL_STALE);
    maps_mode = 3;
    deliver(44100, 27, 1);
    assert(!d->track.phrase_count && d->track.phrases_info.availability == AZ_AVAIL_EMPTY);
    maps_mode = 4;
    deliver(44100, 27, 1);
    assert(!d->track.beat_count && d->track.grid_info.availability == AZ_AVAIL_STALE);
    assert(!d->track.phrase_count && d->track.phrases_info.availability == AZ_AVAIL_STALE);
    maps_mode = 5;
    deliver(44100, 27, 1);
    assert(d->track.beat_count == 2 && d->grid_revision == 8 && d->track.phrase_count == 1);
}
int main(void)
{
    assert(az_mod_v1_entry()->on_frame == NULL &&
           !strcmp(az_mod_v1_entry()->id, "cues"));
    assert(init(NULL) == -1);
    config.enabled = 1;
    live = (az_deck_snapshot){ .size = sizeof live,
                               .version = AZ_DATA_VERSION,
                               .deck = 0,
                               .valid = AZ_LIVE_LOADED | AZ_LIVE_NATIVE_LOAD,
                               .loaded = 1,
                               .native_load_generation = 15,
                               .native_load_unit_generation = 8,
                               .native_load_observed_ns = 1 };
    status_track = (az_track_ref){ .size = sizeof status_track,
                                   .version = AZ_DATA_VERSION,
                                   .source_id = 1,
                                   .source_generation = 8,
                                   .library_id = 9,
                                   .track_id = 10 };
    bank = (az_native_cue_snapshot){ .size = sizeof bank,
                                     .version = AZ_DATA_VERSION,
                                     .native_load_generation = 15,
                                     .revision = 100,
                                     .source_rate_hz = 48000 };
    assert(init(&core) == 0 && threads == 1 && listener);
    deliver(0, 15, 1);
    step();
    assert(!submissions && !atomic_load(&jobs[0].phase));
    phrase_available = 0;
    deliver(48000, 15, 1);
    step();
    assert(!atomic_load(&jobs[0].phase));
    phrase_available = 1;
    load_provenance = AZ_PROV_FILE;
    deliver(44100, 15, 1);
    step();
    assert(!decks[0].loaded && !atomic_load(&jobs[0].phase) && !submissions);
    load_provenance = AZ_PROV_NATIVE;
    deliver(44100, 15, 1);
    /* A native replacement can precede its status delivery. An otherwise
     * usable old status cannot target that new generation. */
    live.native_load_generation = bank.native_load_generation = 16;
    step();
    assert(!atomic_load(&jobs[0].phase) && !submissions);
    live.native_load_generation = bank.native_load_generation = 15;
    step();
    assert(atomic_load(&jobs[0].phase) == 1 && jobs[0].rate == 48000);
    assert(worker_step());
    step();
    assert(submissions == 1 && decks[0].pending);
    assert(submitted[0].operation == AZ_CMD_CUE_POINT_SET &&
           submitted[0].entry_id == 2);
    assert(submitted[0].flags == WRITE_FLAGS &&
           submitted[0].expected_revision == 100);
    assert(compare_timestamps(submitted[0].position_s, 0., 1, 48000, 1) ==
           AZ_TIME_EQUAL);
    step();
    assert(submissions == 1);
    az_command_completion c = terminal(AZ_COMMAND_APPLIED, 101);
    c.settled = 0;
    callback(&c, callback_user);
    assert(!acknowledgements);
    c.settled = 1;
    acknowledge_busy = 1;
    callback(&c, callback_user);
    assert(acknowledgements == 1 && decks[0].acknowledge);
    callback(&c, callback_user);
    assert(acknowledgements == 1); /* Duplicate terminal. */
    step();
    assert(acknowledgements == 2 && !decks[0].acknowledge);
    az_command_completion wrong = c;
    wrong.result.revision = 99;
    release(wrong);
    assert(decks[0].pending);
    release(c);
    assert(!decks[0].pending);
    step();
    assert(submissions == 1 &&
           decks[0].plan_ready); /* Cache is behind receipt. */
    bank.revision = 101;
    step();
    assert(submissions == 2);
    assert(submitted[1].operation == AZ_CMD_CUE_LOOP_SET &&
           submitted[1].entry_id == 0);
    assert(submitted[1].expected_revision == 101 &&
           !strcmp(submitted[1].comment, "Drop"));
    assert(compare_timestamps(submitted[1].end_s, 8., 1, 48000, 1) ==
           AZ_TIME_EQUAL);
    c = terminal(AZ_COMMAND_UNCERTAIN, 0);
    callback(&c, callback_user);
    release(c);
    assert(decks[0].uncertain_load == 15 && !decks[0].plan_ready);
    config_changed();
    step();
    assert(submissions == 2 && !atomic_load(&jobs[0].phase));
    /* Replacement load restores eligibility and fences the old source. */
    live.native_load_generation = bank.native_load_generation = 16;
    bank.revision = 200;
    deliver(48000, 16, 1);
    step();
    assert(worker_step());
    refusals = 1;
    step();
    assert(submissions == 2 && !decks[0].pending);
    step();
    assert(submissions == 3 && submitted[2].expected_load_generation == 16);
    config.enabled = 0;
    config_changed();
    assert(cancellations == 1 && decks[0].obsolete);
    c = terminal(AZ_COMMAND_APPLIED, 201);
    callback(&c, callback_user);
    release(c);
    step();
    assert(submissions == 3 && !decks[0].plan_ready && !decks[0].pending);
    config.enabled = 1;
    config_changed();
    deliver(48000, 16, 0);
    step();
    assert(!decks[0].loaded);
    /* Auto never changes a track containing any hot or memory cue. Manual can
     * replace the whole bank only after a source/revision-qualified yes. */
    live.native_load_generation = bank.native_load_generation = 17;
    bank.revision = 300;
    bank.count = 1;
    bank.entries[0] = (az_cue_entry){ .size = sizeof(az_cue_entry),
                                      .version = AZ_DATA_VERSION,
                                      .valid = AZ_CUE_START,
                                      .classification = AZ_CUE_HOT,
                                      .slot = 1,
                                      .entry_id = 1,
                                      .type = AZ_CUE_POINT,
                                      .start_s = 2 };
    deliver(48000, 17, 1);
    step();
    assert(submissions == 3 && !atomic_load(&jobs[0].phase));
    assert(decks[0].auto_done_epoch == 17 &&
           strstr(shown, "existing cues preserved"));
    config.enabled = 0;
    config_changed();
    manual_apply(0);
    step();
    assert(worker_step());
    step();
    assert(confirmations == 1 && submissions == 3 && decks[0].confirming);
    confirm_callback(0, confirm_user);
    step();
    assert(submissions == 3 && !decks[0].manual_requested);
    manual_apply(0);
    step();
    assert(worker_step());
    step();
    assert(confirmations == 2);
    bank.revision = 301;
    confirm_callback(1, confirm_user);
    step();
    assert(submissions == 3 && !decks[0].plan_ready);
    manual_apply(0);
    step();
    assert(worker_step());
    step();
    assert(confirmations == 3);
    confirm_callback(1, confirm_user);
    step();
    assert(submissions == 4);
    assert(submitted[3].operation == AZ_CMD_HOT_CLEAR &&
           submitted[3].flags == WRITE_FLAGS &&
           submitted[3].expected_revision == 301);
    c = terminal(AZ_COMMAND_APPLIED, 302);
    callback(&c, callback_user);
    release(c);
    bank.count = 0;
    bank.revision = 302;
    deliver(48000, 17, 1);
    step();
    assert(submissions == 5);
    assert(submitted[4].operation == AZ_CMD_CUE_POINT_SET &&
           submitted[4].expected_revision == 302);
    c = terminal(AZ_COMMAND_APPLIED, 303);
    callback(&c, callback_user);
    release(c);
    bank.revision = 303;
    step();
    assert(submissions == 6 && submitted[5].operation == AZ_CMD_CUE_LOOP_SET);
    c = terminal(AZ_COMMAND_APPLIED, 304);
    callback(&c, callback_user);
    release(c);
    bank.revision = 304;
    step();
    assert(!decks[0].plan_ready && decks[0].saved == 2);
    /* Completion for the old track cannot mark the new load as processed. */
    manual_apply(0);
    step();
    assert(worker_step());
    step();
    assert(submissions == 7);
    live.native_load_generation = bank.native_load_generation = 18;
    bank.revision = 400;
    deliver(48000, 18, 1);
    assert(decks[0].obsolete);
    c = terminal(AZ_COMMAND_CANCELLED, 0);
    callback(&c, callback_user);
    release(c);
    assert(decks[0].auto_done_epoch == 0 && !decks[0].pending);
    config.enabled = 1;
    config_changed();
    step();
    assert(atomic_load(&jobs[0].phase) == 1);
    /* Applying a new config during an uncertain old request cannot remove the
     * same-load retry fence, even when that request has become obsolete. */
    assert(worker_step());
    step();
    assert(submissions == 8);
    config_changed();
    c = terminal(AZ_COMMAND_UNCERTAIN, 0);
    callback(&c, callback_user);
    release(c);
    step();
    assert(decks[0].uncertain_load == 18 && submissions == 8 &&
           !atomic_load(&jobs[0].phase));
    /* Empty replacement plans preserve the original bank without even asking.
     */
    live.native_load_generation = bank.native_load_generation = 19;
    bank.revision = 500;
    bank.count = 1;
    bank.entries[0] = (az_cue_entry){ .size = sizeof(az_cue_entry),
                                      .version = AZ_DATA_VERSION,
                                      .valid = AZ_CUE_START,
                                      .classification = AZ_CUE_HOT,
                                      .slot = 1,
                                      .entry_id = 1,
                                      .type = AZ_CUE_POINT,
                                      .start_s = 2 };
    deliver(48000, 19, 1);
    step();
    planned_count = 0;
    manual_apply(0);
    step();
    assert(worker_step());
    step();
    assert(submissions == 8 && confirmations == 3 && bank.count == 1 &&
           !decks[0].plan_ready);
    /* A memory-only bank is just as protected as a hot bank on automatic load.
     */
    live.native_load_generation = bank.native_load_generation = 20;
    bank.revision = 600;
    bank.entries[0].classification = AZ_CUE_MEMORY;
    bank.entries[0].slot = 0;
    deliver(48000, 20, 1);
    step();
    assert(submissions == 8 && decks[0].auto_done_epoch == 20);
    /* A subscription resync is not another track load. Removing all cues
     * after an automatic skip must not enable automatic placement again. */
    bank.count = 0;
    event_kind = AZ_STATUS_RESYNC;
    deliver(48000, 20, 1);
    step();
    assert(decks[0].auto_done_epoch == 20 && !atomic_load(&jobs[0].phase));
    /* Resynchronization can accompany our own Hot deletion. A fresh image
     * of the same load is not a new track and must not abandon confirmed work. */
    submissions = acknowledgements = cancellations = confirmations = 0;
    planned_count = 2;
    config.enabled = 0;
    config_changed();
    event_kind = AZ_STATUS_CHANGE;
    live.native_load_generation = bank.native_load_generation = 21;
    bank.revision = 700;
    bank.count = 4;
    for (unsigned i = 0; i < bank.count; ++i) {
        bank.entries[i] = (az_cue_entry){ .size = sizeof(az_cue_entry),
                                          .version = AZ_DATA_VERSION,
                                          .valid = AZ_CUE_START,
                                          .classification = i < 2 ? AZ_CUE_MEMORY : AZ_CUE_HOT,
                                          .entry_id = i < 2 ? i + 1 : i + 5,
                                          .slot = i < 2 ? 0 : i + 5,
                                          .type = AZ_CUE_POINT,
                                          .start_s = 2. * (i + 1) };
    }
    deliver(44100, 21, 1);
    manual_apply(0);
    step();
    assert(worker_step());
    step();
    assert(confirmations == 1 && decks[0].confirming);
    confirm_callback(1, confirm_user);
    step();
    for (unsigned i = 0; i < 4; ++i) {
        assert(submissions == (int)i + 1 && decks[0].pending);
        assert(submitted[i].operation == (i < 2 ? AZ_CMD_MEMORY_DELETE : AZ_CMD_HOT_CLEAR));
        assert(submitted[i].expected_revision == 700 + i);
        c = terminal(AZ_COMMAND_APPLIED, 701 + i);
        callback(&c, callback_user);
        memmove(bank.entries, bank.entries + 1, (--bank.count) * sizeof bank.entries[0]);
        bank.revision = 701 + i;
        if (i == 2) {
            event_kind = AZ_STATUS_RESYNC;
            deliver(44100, 21, 1);
            assert(!decks[0].obsolete && decks[0].plan_ready &&
                   decks[0].confirmed && decks[0].manual_requested);
            assert(!cancellations);
        }
        release(c);
        deliver(44100, 21, 1);
        event_kind = AZ_STATUS_CHANGE;
        step();
    }
    assert(submissions == 5 && submitted[4].operation == AZ_CMD_CUE_POINT_SET);
    c = terminal(AZ_COMMAND_APPLIED, 705);
    callback(&c, callback_user);
    release(c);
    bank.revision = 705;
    step();
    assert(submissions == 6 && submitted[5].operation == AZ_CMD_CUE_LOOP_SET);
    c = terminal(AZ_COMMAND_APPLIED, 706);
    callback(&c, callback_user);
    release(c);
    bank.revision = 706;
    step();
    assert(decks[0].saved == 2 && !decks[0].plan_ready &&
           !decks[0].manual_requested && confirmations == 1 && !cancellations);
    /* Model the native store retaining the two saved definitions on reload.
     * Automatic placement must not even attempt another SDK write. */
    bank.count = 2;
    for (unsigned i = 0; i < bank.count; ++i) {
        const az_command_request *r = &submitted[4 + i];
        bank.entries[i] = (az_cue_entry){ .size = sizeof(az_cue_entry),
                                          .version = AZ_DATA_VERSION,
                                          .valid = AZ_CUE_START | AZ_CUE_SAVED | AZ_CUE_COLOR |
                                                   AZ_CUE_COMMENT | (i ? AZ_CUE_END : 0),
                                          .classification = r->classification,
                                          .slot = r->classification == AZ_CUE_HOT ? r->entry_id : 0,
                                          .entry_id = r->classification == AZ_CUE_HOT ? r->entry_id : 1,
                                          .type = i ? AZ_CUE_LOOP : AZ_CUE_POINT,
                                          .start_s = r->position_s,
                                          .end_s = r->end_s,
                                          .rgb = r->rgb,
                                          .saved = 1 };
        snprintf(bank.entries[i].comment, sizeof bank.entries[i].comment,
                 "%s", r->comment);
    }
    az_cue_entry saved[2];
    memcpy(saved, bank.entries, sizeof saved);
    config.enabled = 1;
    config_changed();
    live.native_load_generation = bank.native_load_generation = 22;
    bank.revision = 800;
    deliver(44100, 22, 1);
    uint64_t attempts = next_request;
    for (int i = 0; i < 4; ++i) {
        step();
        event_kind = AZ_STATUS_RESYNC;
        deliver(44100, 22, 1);
    }
    assert(submissions == 6 && next_request == attempts &&
           decks[0].auto_done_epoch == 22 && !atomic_load(&jobs[0].phase));
    assert(bank.count == 2 && strstr(shown, "existing cues preserved"));
    for (unsigned i = 0; i < bank.count; ++i) {
        const az_cue_entry *actual = &bank.entries[i], *prior = &saved[i];
        assert(actual->classification == prior->classification &&
               actual->slot == prior->slot && actual->type == prior->type &&
               actual->rgb == prior->rgb && actual->valid == prior->valid &&
               actual->saved == 1 && actual->modified == 0 &&
               !strcmp(actual->comment, prior->comment));
        int order = compare_timestamps(actual->start_s, prior->start_s, 1,
                                       bank.source_rate_hz, 1);
        assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
        if (actual->valid & AZ_CUE_END) {
            order = compare_timestamps(actual->end_s, prior->end_s, 1,
                                       bank.source_rate_hz, 1);
            assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
        }
    }
    /* A saved loop alone protects the track, for either cue collection. */
    bank.count = 1;
    bank.entries[0] = saved[1];
    for (uint64_t epoch = 23; epoch <= 24; ++epoch) {
        if (epoch == 24) {
            bank.entries[0].classification = AZ_CUE_HOT;
            bank.entries[0].slot = bank.entries[0].entry_id = 3;
        }
        live.native_load_generation = bank.native_load_generation = epoch;
        bank.revision++;
        deliver(44100, epoch, 1);
        step();
        assert(submissions == 6 && next_request == attempts &&
               decks[0].auto_done_epoch == epoch && !atomic_load(&jobs[0].phase));
        assert(bank.count == 1 && bank.entries[0].type == AZ_CUE_LOOP);
    }
    /* Streaming/native loads need no USB reference. Unit metadata without a
     * qualified join stays unavailable; load-scoped metadata remains usable. */
    qualified_track = 0;
    bank.count = 0;
    for (uint64_t epoch = 25; epoch <= 26; ++epoch) {
        load_metadata = 0;
        if (epoch == 26) {
            config.group_count = 1;
            config.groups[0].enabled = config.groups[0].rule_count = 1;
            snprintf(config.groups[0].artist, sizeof config.groups[0].artist,
                     "Test artist");
        }
        live.native_load_generation = bank.native_load_generation = epoch;
        bank.revision++;
        deliver(44100, epoch, 1);
        assert(decks[0].loaded && !decks[0].track.track.track_id);
        if (epoch == 26) {
            step();
            assert(!atomic_load(&jobs[0].phase) && submissions == 7);
            assert(strstr(shown, "waiting for rule filter metadata"));
            load_metadata = 1;
            deliver(44100, epoch, 1);
        }
        uint64_t revision = decks[0].revision;
        step();
        assert(atomic_load(&jobs[0].phase) == 1);
        deliver(44100, epoch, 1);
        assert(decks[0].revision == revision); /* Same load keeps its plan. */
        assert(worker_step());
        step();
        assert(submissions == (int)(epoch - 18) &&
               submitted[submissions - 1].expected_load_generation == epoch &&
               submitted[submissions - 1].flags == WRITE_FLAGS);
        c = terminal(AZ_COMMAND_UNCERTAIN, 0);
        callback(&c, callback_user);
        release(c);
    }
    /* A late USB reference/metadata join cannot truncate a plan that already
     * started writing for this native load. All remaining cue fences stay live. */
    config.group_count = 0;
    load_metadata = 0;
    live.native_load_generation = bank.native_load_generation = 27;
    bank.revision = 900;
    deliver(44100, 27, 1);
    step();
    assert(worker_step());
    step();
    assert(submissions == 9 && decks[0].pending && decks[0].plan_ready);
    qualified_track = 1;
    deliver(44100, 27, 1);
    assert(decks[0].pending && decks[0].plan_ready && !decks[0].obsolete);
    c = terminal(AZ_COMMAND_APPLIED, 901);
    callback(&c, callback_user);
    release(c);
    bank.revision = 901;
    step();
    assert(submissions == 10 && submitted[9].expected_load_generation == 27 &&
           submitted[9].expected_revision == 901);
    c = terminal(AZ_COMMAND_UNCERTAIN, 0);
    callback(&c, callback_user);
    release(c);
    picker_interaction();
    analysis_receipts();
    puts(
        "Runtime passed: public SDK commands, native load fences, source rate, "
        "async cleanup, cache lag, disable and uncertainty");
}
