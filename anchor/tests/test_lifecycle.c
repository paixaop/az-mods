/* SPDX-License-Identifier: GPL-3.0-only */
/* Drive the registered callback with the SDK's borrowed, load-scoped views. */
#include <assert.h>
#include <stdint.h>
#include <time.h>

static uint64_t fixture_ms = UINT64_C(5000000000);

static int fixture_clock(clockid_t clock, struct timespec *ts)
{
    assert(clock == CLOCK_MONOTONIC);
    ts->tv_sec = (time_t)(fixture_ms / 1000);
    ts->tv_nsec = (long)(fixture_ms % 1000) * 1000000;
    return 0;
}

#define clock_gettime fixture_clock
#include "anchor.c"
#undef clock_gettime

struct az_surface {
    az_surface_v1 desc;
    az_rect area;
    int shown;
    unsigned redraws;
};

static az_surface surfaces[2];
static unsigned nsurfaces;
static az_deck_status_fn listener;
static az_deck_watch_v1 watch;
static void *listener_user;

static az_surface *create(const char *id, const az_surface_v1 *desc)
{
    assert(!strcmp(id, MOD_ID) && nsurfaces < 2);
    assert(desc->views == AZ_VIEW_TWO_DECKS && desc->on_input && desc->on_paint);
    surfaces[nsurfaces].desc = *desc;
    return &surfaces[nsurfaces++];
}

static void place(az_surface *s, az_rect r) { s->area = r; }
static void show(az_surface *s, int on) { s->shown = on; }
static void redraw(az_surface *s) { s->redraws++; }
static void redraw_page(void) {}
static void draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t color)
{
    (void)d;
    (void)x;
    (void)y;
    (void)s;
    (void)px;
    (void)color;
}
static void fill(az_draw *d, int x, int y, int w, int h, uint32_t color)
{
    (void)d;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
}
static void rect(az_draw *d, int x, int y, int w, int h, int line, uint32_t color)
{
    (void)line;
    fill(d, x, y, w, h, color);
}
static int image(az_draw *d, int x, int y, const unsigned char *rgba, int w, int h)
{
    (void)rgba;
    fill(d, x, y, w, h, 0);
    return 0;
}
static int width(const char *s, int px) { return (int)strlen(s) * px / 2; }
static uint32_t color(int role)
{
    (void)role;
    return 0xffffff;
}

static int subscribe(const char *id, const az_deck_watch_v1 *w, az_deck_status_fn fn, void *user,
                     az_deck_subscription *out)
{
    assert(!strcmp(id, MOD_ID) && w->size == sizeof *w && w->version == AZ_DATA_VERSION);
    assert(w->deck_mask == 3 && w->position_hz == 30 && !listener);
    listener = fn;
    listener_user = user;
    watch = *w;
    *out = 1;
    return AZ_OK;
}

static int release(const char *id, az_deck_subscription handle)
{
    (void)id;
    (void)handle;
    return AZ_OK;
}

static const az_library_v1 *library(uint32_t abi)
{
    static const az_library_v1 lib = {
        .size = sizeof lib,
        .abi = AZ_LIBRARY_ABI,
        .on_deck_status = subscribe,
        .deck_status_release = release,
    };
    return abi == AZ_LIBRARY_ABI ? &lib : NULL;
}

static const az_core_v1 core = {
    .size = sizeof core,
    .abi = AZ_MOD_ABI,
    .library_service = library,
    .surface_create = create,
    .surface_show = show,
    .surface_place = place,
    .surface_redraw = redraw,
    .request_redraw = redraw_page,
    .draw_text = draw_text,
    .fill_rect = fill,
    .draw_rect = rect,
    .draw_image = image,
    .text_width = width,
    .color = color,
};

typedef struct {
    uint64_t epoch, seq;
    unsigned deck, rate;
    az_status_load_v1 load;
    az_status_transport_v1 transport;
    az_status_position_v1 position;
    az_status_tempo_v1 tempo;
    az_status_grid_v1 grid;
    az_status_phrases_v1 phrases;
    az_status_cues_v1 cues;
    az_status_loop_v1 loop;
    az_beat beats[257];
    az_phrase rows[4];
    az_cue_entry hot[2], memory;
} fixture_t;

static void receipt(fixture_t *f, az_status_topic_v1 *topic, unsigned field,
                    az_status_field_info_v1 *info, unsigned availability, unsigned count)
{
    uint64_t bit = az_status_field_bit(field);
    if (availability != AZ_AVAIL_UNSUPPORTED) topic->supported |= bit;
    else topic->supported &= ~bit;
    if (availability == AZ_AVAIL_READY) topic->valid |= bit;
    else topic->valid &= ~bit;
    *info = (az_status_field_info_v1){
        .info = { availability, AZ_PROV_NATIVE, f->seq + 1, fixture_ms * 1000000 },
        .scope = AZ_STATUS_LOAD_SCOPE,
        .scope_id = f->epoch,
        .count = count,
        .provider_id = f->deck + 10,
    };
}

#define FIELD(f, topic, field, selector, availability, count) \
    receipt((f), &(f)->topic.header, AZ_STATUS_##selector, &(f)->topic.field##_info, availability, count)

static az_status_coord_v1 coordinate(const fixture_t *f, double seconds)
{
    return (az_status_coord_v1){ .seconds = seconds, .samples = llround(seconds * f->rate), .rate_hz = f->rate };
}

static void head(fixture_t *f, double seconds)
{
    f->position.head = coordinate(f, seconds);
    FIELD(f, position, head, POSITION_HEAD, AZ_AVAIL_READY, 1);
}

static void playback(fixture_t *f, int state)
{
    f->transport.state = state;
    f->transport.direction = state == AZ_PLAYBACK_PLAYING ? AZ_STATUS_DIRECTION_FORWARD : AZ_STATUS_DIRECTION_STATIONARY;
    f->transport.rate = state == AZ_PLAYBACK_PLAYING ? 1.0 : 0;
    FIELD(f, transport, state, TRANSPORT_STATE, AZ_AVAIL_READY, 1);
    FIELD(f, transport, direction, TRANSPORT_DIRECTION, AZ_AVAIL_READY, 1);
    FIELD(f, transport, rate, TRANSPORT_RATE, AZ_AVAIL_READY, 1);
}

static void loop_bounds(fixture_t *f, int engaged, double in, double out)
{
    f->loop.engaged = engaged;
    f->loop.working_in = coordinate(f, in);
    f->loop.working_out = coordinate(f, out);
    FIELD(f, loop, engaged, LOOP_ENGAGED, AZ_AVAIL_READY, 1);
    FIELD(f, loop, working_in, LOOP_WORKING_IN, engaged ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY, engaged);
    FIELD(f, loop, working_out, LOOP_WORKING_OUT, engaged ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY, engaged);
}

static void link_phrases(fixture_t *f)
{
    az_status_field_info_v1 *info = &f->grid.current_info;
    f->phrases.current_grid = (az_status_grid_ref_v1){
        f->epoch,
        info->provider_id,
        info->info.revision,
        info->info.provenance,
        AZ_STATUS_GRID_CURRENT,
    };
    FIELD(f, phrases, current_grid, PHRASES_CURRENT_GRID, AZ_AVAIL_READY, 1);
}

static void loaded(fixture_t *f, unsigned deck, uint64_t epoch, int state, double pos)
{
    uint64_t seq = f->seq;
    *f = (fixture_t){ .deck = deck, .epoch = epoch, .seq = seq, .rate = deck ? 48000 : 44100 };
#define HEADER(topic) f->topic.header = (az_status_topic_v1){ .size = sizeof f->topic, .version = AZ_DATA_VERSION }
    HEADER(load);
    HEADER(transport);
    HEADER(position);
    HEADER(tempo);
    HEADER(grid);
    HEADER(phrases);
    HEADER(cues);
    HEADER(loop);
#undef HEADER
    f->load.state = AZ_STATUS_LOAD_LOADED;
    FIELD(f, load, state, LOAD_STATE, AZ_AVAIL_READY, 1);
    f->load.state_info.scope = AZ_STATUS_UNIT;
    f->load.state_info.scope_id = deck + 1;
    playback(f, state);
    head(f, pos);
    loop_bounds(f, 0, 0, 0);
    f->tempo.current_bpm = 120;
    FIELD(f, tempo, current_bpm, TEMPO_CURRENT_BPM, AZ_AVAIL_READY, 1);
    f->tempo.current_bpm_info.scope = AZ_STATUS_UNIT;
    f->tempo.current_bpm_info.scope_id = deck + 1;
    f->tempo.original_bpm = 120;
    FIELD(f, tempo, original_bpm, TEMPO_ORIGINAL_BPM, AZ_AVAIL_READY, 1);

    for (unsigned i = 0; i < 257; i++) f->beats[i] = (az_beat){ i * .5, 120, i % 4 + 1, 0 };
    for (unsigned i = 0; i < 4; i++) f->rows[i] = (az_phrase){ i * 64, (i + 1) * 64, i % 2 ? 5 : 2, 1, 0, 0 };
    f->grid.current = f->beats;
    FIELD(f, grid, current, GRID_CURRENT, AZ_AVAIL_READY, 257);
    f->phrases.current = f->rows;
    FIELD(f, phrases, current, PHRASES_CURRENT, AZ_AVAIL_READY, 4);
    link_phrases(f);

    f->hot[0] = (az_cue_entry){ .size = sizeof *f->hot, .version = AZ_DATA_VERSION, .valid = AZ_CUE_START, .classification = AZ_CUE_HOT, .slot = 3, .start_s = 80 };
    f->hot[1] = f->hot[0];
    f->hot[1].slot = 1;
    f->hot[1].start_s = 0;
    f->memory = (az_cue_entry){ .size = sizeof f->memory, .version = AZ_DATA_VERSION, .valid = AZ_CUE_START, .classification = AZ_CUE_MEMORY, .entry_id = 1, .start_s = 10 };
    f->cues.live_hot = f->hot;
    f->cues.live_memory = &f->memory;
    FIELD(f, cues, live_hot, CUES_LIVE_HOT, AZ_AVAIL_READY, 2);
    FIELD(f, cues, live_memory, CUES_LIVE_MEMORY, AZ_AVAIL_READY, 1);
}

static void publish(fixture_t *f, unsigned kind, uint64_t topics)
{
    az_deck_status_view_v1 view = {
        .size = sizeof view,
        .version = AZ_DATA_VERSION,
        .load = &f->load,
        .transport = &f->transport,
        .position = &f->position,
        .tempo = &f->tempo,
        .grid = &f->grid,
        .phrases = &f->phrases,
        .cues = &f->cues,
        .loop = &f->loop,
    };
    az_deck_status_event_v1 e = {
        .size = sizeof e,
        .version = AZ_DATA_VERSION,
        .kind = kind,
        .deck = f->deck,
        .view = &view,
        .subscription = 1,
        .sequence = ++f->seq,
        .load_epoch = f->epoch,
        .topics = watch.topics,
        .changed_topics = topics,
        .published_ns = fixture_ms * 1000000,
        .lost_topics = kind == AZ_STATUS_RESYNC ? topics : 0,
        .loss_reason = kind == AZ_STATUS_RESYNC ? AZ_STATUS_CAPTURE_GAP : 0,
    };
    listener(&e, listener_user);
}

static void expect_ms(uint64_t first, uint64_t second, unsigned rate)
{
    int order = compare_timestamps(first, second, 1000, rate, 1);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
}

static void missing_data_and_fallbacks(void)
{
    fixture_t a = { 0 }, b = { 0 };
    loaded(&a, 0, 100, AZ_PLAYBACK_PLAYING, 1);
    loaded(&b, 1, 101, AZ_PLAYBACK_PAUSED, 16);
    publish(&a, AZ_STATUS_INITIAL, watch.topics);
    publish(&b, AZ_STATUS_INITIAL, watch.topics);
    settings_set(SET_RULE, AN_RULE_PHRASE);
    FIELD(&a, phrases, current, PHRASES_CURRENT, AZ_AVAIL_LOADING, 0);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_PHRASES));
    assert(plan.notice == AN_NOTICE_READING && strstr(plan.line2, "phrase map"));
    FIELD(&a, phrases, current, PHRASES_CURRENT, AZ_AVAIL_EMPTY, 0);
    FIELD(&a, cues, live_hot, CUES_LIVE_HOT, AZ_AVAIL_LOADING, 0);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) | AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    assert(plan.notice == AN_NOTICE_READING && strstr(plan.line2, "Hot Cues"));
    FIELD(&a, cues, live_hot, CUES_LIVE_HOT, AZ_AVAIL_EMPTY, 0);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    assert(plan.notice == AN_NOTICE_ANALYSIS_A);
    settings_set(SET_RULE, AN_RULE_HOTCUE);
    assert(plan.notice == AN_NOTICE_NO_CUES);
    FIELD(&a, cues, live_hot, CUES_LIVE_HOT, AZ_AVAIL_FAILED, 0);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    assert(plan.notice == AN_NOTICE_CUES_UNAVAILABLE);
    settings_set(SET_RULE, AN_RULE_GRID16);
    FIELD(&a, grid, current, GRID_CURRENT, AZ_AVAIL_LOADING, 0);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_GRID));
    assert(plan.notice == AN_NOTICE_READING && strstr(plan.line2, "beat grid"));
    FIELD(&a, grid, current, GRID_CURRENT, AZ_AVAIL_EMPTY, 0);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_GRID));
    assert(plan.notice == AN_NOTICE_ANALYSIS_A);

    /* Saved maps work only with a saved-grid association from the same load. */
    a.grid.saved = a.beats;
    a.phrases.saved = a.rows;
    FIELD(&a, grid, saved, GRID_SAVED, AZ_AVAIL_READY, 257);
    FIELD(&a, phrases, saved, PHRASES_SAVED, AZ_AVAIL_READY, 4);
    a.phrases.saved_grid = (az_status_grid_ref_v1){ a.epoch, a.grid.saved_info.provider_id,
                                                    a.grid.saved_info.info.revision, AZ_PROV_NATIVE, AZ_STATUS_GRID_SAVED };
    FIELD(&a, phrases, saved_grid, PHRASES_SAVED_GRID, AZ_AVAIL_READY, 1);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES));
    assert(decks[0].analysis.nphrases == 4 && !decks[0].phrases_pending);
    settings_set(SET_RULE, AN_RULE_PHRASE);
    open_selector();
    assert(sel_open && sel_n >= 4 && sel_deck == 0);
    latch_candidate(0);
    assert(!sel_open && latched_ms && plan.valid);

    /* Incoming target acquisition reports missing and pending analyses distinctly. */
    loaded(&b, 1, 102, AZ_PLAYBACK_PAUSED, 16);
    FIELD(&b, phrases, current, PHRASES_CURRENT, AZ_AVAIL_LOADING, 0);
    publish(&b, AZ_STATUS_INITIAL, watch.topics);
    assert(!b_targets[1].valid && plan.notice == AN_NOTICE_READING);
    FIELD(&b, phrases, current, PHRASES_CURRENT, AZ_AVAIL_EMPTY, 0);
    FIELD(&b, cues, live_hot, CUES_LIVE_HOT, AZ_AVAIL_EMPTY, 0);
    FIELD(&b, cues, live_memory, CUES_LIVE_MEMORY, AZ_AVAIL_EMPTY, 0);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) | AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    assert(!b_targets[1].valid && plan.notice == AN_NOTICE_ANALYSIS_B);
    FIELD(&b, cues, live_hot, CUES_LIVE_HOT, AZ_AVAIL_LOADING, 0);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    assert(plan.notice == AN_NOTICE_READING);

    settings_set(SET_DISPLAY, ANCHOR_LAYOUT_SIDEBAR - 1);
    playback(&a, AZ_PLAYBACK_PAUSED);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    assert(plan.notice == AN_NOTICE_START_OUTGOING);
    playback(&a, AZ_PLAYBACK_UNKNOWN);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    assert(plan.notice == AN_NOTICE_DECK_STATE);

    /* Bounded captions retain their ellipsis and never overrun the given width. */
    char text[160];
    fit_text(text, sizeof text, "A deliberately long target label", 12, 72);
    assert(strstr(text, "...") && width(text, 12) <= 72);
    fit_text(text, 5, "Long label", 12, 18);
    assert(!strcmp(text, "..."));
    fit_text(text, sizeof text, NULL, 12, 100);
    assert(!text[0]);
    assert(accent_rgb(2) == 0xffb340 && accent_rgb(3) == 0xff453a);
}

int main(void)
{
    const az_mod_v1 *mod = az_mod_v1_entry();
    assert(!mod->tick && !mod->on_frame);
    az_core_v1 no_status = core;
    no_status.library_service = NULL;
    assert(mod->init(&no_status) != AZ_OK);
    assert(mod->init(&core) == AZ_OK && listener && nsurfaces == 2);
    assert(!strip_shown && !tab_shown);

    fixture_t a = { 0 }, b = { 0 };
    loaded(&a, 0, 1, AZ_PLAYBACK_PLAYING, 1);
    loaded(&b, 1, 2, AZ_PLAYBACK_PAUSED, 16);
    publish(&a, AZ_STATUS_INITIAL, watch.topics);
    publish(&b, AZ_STATUS_INITIAL, watch.topics);
    assert(strip_shown && cur_a == 0 && cur_b == 1 && plan.state == AN_ST_SINGLE);
    expect_ms(plan.b_target_ms, 32000, b.rate);
    expect_ms(plan.action_wall_ms, fixture_ms + 15000, 1000);

    /* Analysis belongs to the callback's cut, even after its arrays are reused. */
    b.beats[64].time_s = 35;
    b.hot[1].start_s = 2;
    open_b_selector(0);
    assert(sel_n == 4);
    expect_ms(sel_cand[1].ms, 32000, b.rate);
    open_b_selector(1);
    assert(sel_n == 2);
    expect_ms(sel_cand[0].ms, 0, b.rate);
    expect_ms(sel_cand[1].ms, 80000, b.rate);
    close_selector();
    b.beats[64].time_s = 32;
    b.hot[1].start_s = 0;

    /* Prepared loops change PLAY immediately while B is paused. */
    loop_bounds(&b, 1, 16, 24);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    assert(plan.state == AN_ST_START && plan.loop_prepared && plan.loops == 2);
    expect_ms(plan.action_wall_ms, fixture_ms + 7000, 1000);
    loop_bounds(&b, 1, 16, 20);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    assert(plan.state == AN_ST_START && plan.loops == 4);
    expect_ms(plan.action_wall_ms, fixture_ms + 3000, 1000);
    FIELD(&b, loop, working_out, LOOP_WORKING_OUT, AZ_AVAIL_LOADING, 0);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    assert(plan.notice == AN_NOTICE_LOOP_UNKNOWN && !plan.action && !plan.play_now);
    loop_bounds(&b, 1, 16, 20);
    b.loop.working_out_info.scope_id = a.epoch;
    publish(&b, AZ_STATUS_RESYNC, AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    assert(!decks[1].loop.valid && !plan.action && !plan.play_now);
    loop_bounds(&b, 1, 0, 4);
    head(&b, 0);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_LOOP) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    assert(decks[1].loop.valid);
    expect_ms(decks[1].loop.in_ms, 0, b.rate);
    expect_ms(plan.b_target_ms, 32000, b.rate);

    /* The final pass opens after the head reaches IN, with loop-out-to-target travel. */
    head(&b, 16);
    loop_bounds(&b, 1, 16, 20);
    playback(&b, AZ_PLAYBACK_PLAYING);
    latched_ms = 33000;
    snprintf(latched_label, sizeof latched_label, "PICKED TARGET");
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_LOOP) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    assert(plan.state == AN_ST_WAIT && plan.looping);
    expect_ms(plan.out_wall_ms, fixture_ms + 20000, 1000);
    fixture_ms += 16000;
    head(&a, 17);
    head(&b, 16);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    assert(plan.state == AN_ST_OPEN && plan.window_open && plan.action == 2);
    loop_bounds(&b, 0, 0, 0);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    assert(plan.state == AN_ST_RELEASED && cur_b == 1);
    publish(&b, AZ_STATUS_RESYNC, AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    assert(!decks[1].loop.released && cur_b == 1);

    /* Live cue edits replace open pickers. A confirmed empty bank hides saved cues. */
    open_b_selector(1);
    b.cues.saved_hot = b.hot;
    FIELD(&b, cues, saved_hot, CUES_SAVED_HOT, AZ_AVAIL_READY, 2);
    FIELD(&b, cues, live_hot, CUES_LIVE_HOT, AZ_AVAIL_EMPTY, 0);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    assert(sel_open && !sel_n && !layout_ok && !sel_pending && b_targets[1].valid);
    FIELD(&b, cues, live_hot, CUES_LIVE_HOT, AZ_AVAIL_READY, 2);
    publish(&b, AZ_STATUS_RESYNC, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    assert(sel_open && sel_n == 2);
    latch_candidate(0);
    assert(b_targets[1].valid);
    expect_ms(b_targets[1].target.ms, 0, b.rate);
    head(&b, 3.217);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    assert(plan.notice == AN_NOTICE_B_TARGET_PASSED);
    open_b_selector(0);
    set_b_here();
    expect_ms(b_targets[1].target.ms, 3217, b.rate);

    /* A grid change cannot reuse a phrase association from the previous revision. */
    a.beats[64].time_s = 32.25;
    FIELD(&a, grid, current, GRID_CURRENT, AZ_AVAIL_READY, 257);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_GRID));
    assert(!decks[0].analysis.nphrases && decks[0].phrases_pending);
    link_phrases(&a);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_PHRASES));
    assert(decks[0].analysis.nphrases == 4);
    expect_ms(decks[0].analysis.phrases[0].end_ms, 32250, a.rate);

    /* Identical track data under a new load clears the target, role and old hitboxes. */
    open_b_selector(1);
    loaded(&b, 1, 3, AZ_PLAYBACK_PLAYING, 4);
    publish(&b, AZ_STATUS_CHANGE, watch.topics);
    assert(!b_targets[1].valid && !sel_open && !layout_ok && cur_b < 0 && !strip_shown);
    latch_candidate(0);
    assert(!b_targets[1].valid);
    playback(&b, AZ_PLAYBACK_PAUSED);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    assert(strip_shown && cur_b == 1);
    expect_ms(plan.b_target_ms, 32000, b.rate);

    /* An already loaded track can lose its head or direction during recovery. */
    FIELD(&a, position, head, POSITION_HEAD, AZ_AVAIL_STALE, 0);
    publish(&a, AZ_STATUS_RESYNC, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    assert(plan.notice == AN_NOTICE_DECK_STATE && !plan.action);
    head(&a, 1);
    publish(&a, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
    assert(plan.action == 1);
    b.tempo.current_bpm = 121;
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_TEMPO));
    assert(plan.tempo_mismatch);
    b.tempo.current_bpm = 120;
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_TEMPO));
    assert(!plan.tempo_mismatch && plan.action == 1);
    FIELD(&b, loop, engaged, LOOP_ENGAGED, AZ_AVAIL_UNSUPPORTED, 0);
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
    assert(plan.notice == AN_NOTICE_LOOP_UNAVAILABLE && !plan.action);
    b.epoch = 0;
    b.load.state = AZ_STATUS_LOAD_EMPTY;
    publish(&b, AZ_STATUS_CHANGE, AZ_STATUS_TOPIC(AZ_STATUS_LOAD));
    assert(!decks[1].loaded && !b_targets[1].valid && !strip_shown);

    missing_data_and_fallbacks();

    puts("Deck events passed: load epochs, prepared loops, EXIT timing, borrowed maps, cue edits and resync");
}
