/* SPDX-License-Identifier: GPL-3.0-only
 * Track reload / eject regressions against the production lifecycle code. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define an_loaded_read fixture_loaded_read
#define an_resolve_analysis fixture_resolve_analysis
#include "anchor.c"
#undef an_loaded_read
#undef an_resolve_analysis

static az_deck_info live[4];
static unsigned sample[4];
static int reads, cue_reads, eject_during_read, replace_during_read;
static int late_phrases, cue_failure;
static int transient_info, transient_after_read;
static int early_state_attempts;
static int fake_surface;

static int info(int d, az_deck_info *out)
{
    if (transient_info) { transient_info--; return -1; }
    *out = live[d]; return 0;
}
static int view(void) { return AZ_VIEW_DECK; }
static int count(void) { return 2; }
static void show(az_surface *s, int on) { (void)s; (void)on; }
static void redraw(az_surface *s) { (void)s; }
static int cfg(const char *id, const char *key, int def) { (void)id; (void)key; return def; }

int fixture_loaded_read(int d, an_anlz_t *out, char *why, size_t n)
{
    (void)why; (void)n;
    reads++;
    if (early_state_attempts) {
        early_state_attempts--;
        snprintf(why,n,"the loaded state was not found");
        return AN_LOADED_RETRY;
    }
    if (transient_after_read) { transient_info = transient_after_read; transient_after_read = 0; }
    memset(out, 0, sizeof *out);
    out->valid = out->has_grid = out->has_phrases = 1;
    if (late_phrases) { out->has_phrases = 0; late_phrases--; }
    out->bpm = 120;
    out->nbeats = 2;
    out->mood = 1;
    out->nphrases = 1;
    out->phrases = calloc(1, sizeof *out->phrases);
    out->phrases[0] = (an_phrase_t){.start_beat = 1, .end_beat = 2, .kind = 5};
    out->beats = calloc(2, sizeof *out->beats);
    out->beats[0].time_ms = sample[d];
    out->beats[1].time_ms = sample[d] + 500;
    if (eject_during_read) {
        eject_during_read = 0;
        live[d].loaded = 0;
        on_deck_event(d, AZ_DECK_EJECTED, NULL);
    }
    if (replace_during_read) {
        replace_during_read = 0;
        live[d].content_id++;
    }
    return 0;
}

int fixture_resolve_analysis(const az_core_v1 *core, const char *id, uint32_t cid,
                             const char *title, const char *artist, an_anlz_t *out,
                             char *why, int n)
{
    (void)core; (void)id; (void)title; (void)artist; (void)why; (void)n;
    cue_reads++;
    memset(out, 0, sizeof *out);
    out->valid = 1;
    out->ncues = 1;
    out->cues = calloc(1, sizeof *out->cues);
    out->cues[0].time_ms = cid * 1000;
    out->cues[0].kind = AN_CUE_HOT;
    if (cue_failure) {
        snprintf(why, (size_t)n, "could not read the hot cue analysis");
        return -1; /* Also verify cleanup when a resolver leaves a partial allocation. */
    }
    return 0;
}

static void reset(void)
{
    static az_core_v1 core;
    core.size = sizeof core;
    core.deck_info = info; core.view = view; core.deck_count = count;
    core.surface_show = show; core.surface_redraw = redraw; core.cfg_get = cfg;
    C = &core;
    settings_init(C);
    for (int d = 0; d < 4; d++) an_anlz_free(&deck_analysis[d]);
    memset(live, 0, sizeof live);
    memset(status, 0, sizeof status);
    memset(latest_seq, 0, sizeof latest_seq);
    memset(queued, 0, sizeof queued);
    memset(last_loaded, 0, sizeof last_loaded);
    memset(load_notified, 0, sizeof load_notified);
    memset(cues_requested, 0, sizeof cues_requested);
    memset(rt, 0, sizeof rt);
    memset(&plan, 0, sizeof plan);
    q_next = reads = cue_reads = eject_during_read = replace_during_read = 0;
    late_phrases = cue_failure = 0;
    transient_info = transient_after_read = 0;
    early_state_attempts = 0;
    cur_a = 0; cur_b = 1;
    latched_ms = sel_n = sel_open = layout_ok = 0;
    cur_layout = ANCHOR_LAYOUT_CARD;
    strip = (az_surface *)&fake_surface;
    strip_shown = 0;
    worker_ok = loaded_supported = 1;
}

static void track(int d, unsigned cid)
{
    live[d].loaded = 1;
    live[d].content_id = cid;
    snprintf(live[d].title, sizeof live[d].title, "Track %u", cid);
    snprintf(live[d].artist, sizeof live[d].artist, "Artist");
    sample[d] = cid * 100;
    note_track_change(d, &live[d]);
}

static request_t next(void)
{
    request_t req;
    assert(take_request(&req));
    req.not_before_ms = 0; /* Test the worker without its hardware settling delay. */
    return req;
}

static void finish(void)
{
    request_t req = next();
    process_request(&req);
}

int main(void)
{
    reset();
    /* A worker already reading an ejected track must never publish its result. */
    latest_seq[0] = 17;
    clear_deck_analysis(0);
    assert(latest_seq[0] != 17);

    /* Reload and Refresh read the source again even with identical id/title. */
    track(0, 1); finish();
    assert(reads == 1 && slot_anlz(0)->beats[0].time_ms == 100);
    sample[0] = 777;
    on_deck_event(0, AZ_DECK_LOADED, NULL);
    assert(!slot_anlz(0));
    note_track_change(0, &live[0]); finish();
    assert(reads == 2 && slot_anlz(0)->beats[0].time_ms == 777);
    sample[0] = 888;
    settings_set(SET_REFRESH, 1);
    tick();
    assert(status[0] == ST_PENDING && !slot_anlz(0));
    finish();
    assert(reads == 3 && slot_anlz(0)->beats[0].time_ms == 888);

    /* Eject before/during a read cannot restore analysis for the retired load. */
    reset(); track(0, 1);
    request_t old = next();
    on_deck_event(0, AZ_DECK_EJECTED, NULL);
    live[0].loaded = 0;
    process_request(&old);
    assert(reads == 0 && status[0] == ST_NONE && !slot_anlz(0));
    track(0, 2); eject_during_read = 1; finish();
    assert(reads == 1 && status[0] == ST_NONE && !slot_anlz(0));

    /* Metadata changing during a read is rejected even before the next UI load event. */
    reset(); track(0, 3); replace_during_read = 1; finish();
    assert(status[0] == ST_FAILED && !slot_anlz(0));
    note_track_change(0, &live[0]); finish();
    assert(status[0] == ST_OK);

    /* Identical per-library ids/titles on two decks never share an analysis allocation. */
    reset(); track(0, 1); sample[0] = 111; finish();
    track(1, 1); sample[1] = 222; finish();
    assert(reads == 2 && slot_anlz(0)->beats != slot_anlz(1)->beats);
    assert(slot_anlz(0)->beats[0].time_ms == 111 && slot_anlz(1)->beats[0].time_ms == 222);

    /* Artist-only identity changes are loads too (including title-only external sources). */
    reset(); track(0, 0); finish();
    snprintf(live[0].artist, sizeof live[0].artist, "Different Artist");
    note_track_change(0, &live[0]); finish();
    assert(reads == 2);

    /* Hot cue mode must request cues on EVERY subsequent track load. */
    reset(); settings_set(SET_RULE, AN_RULE_HOTCUE);
    track(0, 1); finish();
    track(0, 2); finish();
    assert(reads == 2 && cue_reads == 2 && slot_anlz(0)->ncues == 1);
    assert(slot_anlz(0)->cues[0].time_ms == 2000);
    reset(); settings_set(SET_RULE, AN_RULE_HOTCUE);
    track(0, 1); cue_failure = 1; finish();
    assert(status[0] == ST_FAILED && !slot_anlz(0));
    assert(strstr(fail_why[0], "cue analysis"));
    reset(); track(0, 1); late_phrases = 1; finish();
    assert(reads == 2 && cue_reads == 0 && slot_anlz(0)->has_phrases);
    reset(); track(0, 1); transient_info = 2; transient_after_read = 2; finish();
    assert(reads == 1 && status[0] == ST_OK && slot_anlz(0));
    /* Native UI readiness is temporary. A deferred retry must not fail the track or
     * monopolize the resolver while the other deck is already readable. */
    reset(); track(0,1); early_state_attempts=3;
    request_t early=next(); process_request(&early);
    assert(status[0]==ST_PENDING && queued[0] && queue[0].retries==1);
    assert(!take_request(&old));
    track(1,2); request_t other=next(); assert(other.deck==1);
    early_state_attempts=0; process_request(&other);
    assert(status[1]==ST_OK && status[0]==ST_PENDING);
    early_state_attempts=2;
    for(int i=0;i<3;i++) {
        queue[0].not_before_ms=0;
        early=next(); process_request(&early);
    }
    assert(status[0]==ST_OK && slot_anlz(0)->has_phrases && !queued[0]);
    reset(); track(0,1); early_state_attempts=1;
    early=next(); process_request(&early); assert(queued[0]);
    live[0].loaded=0; on_deck_event(0,AZ_DECK_EJECTED,NULL);
    assert(!queued[0] && status[0]==ST_NONE);
    /* A usable grid remains usable when optional cue lookup fails (e.g. a grid target). */
    reset(); settings_set(SET_RULE, AN_RULE_GRID16);
    track(0, 1); late_phrases = 5; cue_failure = 1; finish();
    assert(status[0] == ST_OK && slot_anlz(0)->has_grid && !slot_anlz(0)->has_phrases);

    /* A manually picked phrase overrides the default Hot cue rule on a cue-less track. */
    reset(); track(0, 1); finish(); track(1, 2); finish();
    an_target_opts_t opts = {.rule = AN_RULE_HOTCUE, .hotcue_any = 1};
    latched_ms = 10000; latched_gen = deck_gen[0];
    snprintf(latched_label, sizeof latched_label, "MANUAL PHRASE");
    live[0].bpm = live[1].bpm = 120;
    compute_plan(0, live, 0, 1, &opts);
    assert(plan.state == AN_ST_SINGLE && plan.target_ms == 10000);

    /* Rapid loads coalesce to the newest request instead of overflowing a stale FIFO. */
    reset();
    for (unsigned i = 1; i <= 100; i++) track(0, i);
    request_t latest = next();
    assert(latest.content_id == 100 && !take_request(&old));
    process_request(&latest);
    assert(reads == 1 && status[0] == ST_OK && slot_anlz(0)->beats[0].time_ms == 10000);

    /* A replacement immediately clears old targets, picker, plan and loop detection. */
    sel_open = layout_ok = plan.valid = rt[0].confirmed = 1;
    sel_n = 1; latched_ms = 1000; strip_shown = 1;
    on_deck_event(0, AZ_DECK_LOADED, NULL);
    assert(!sel_open && !sel_n && !layout_ok && !latched_ms && !plan.valid && !rt[0].confirmed);
    assert(!strip_shown && !slot_anlz(0));
    sel_n = 1; sel_deck = 0; sel_gen = deck_gen[0] - 1;
    sel_cand[0].ms = 1234;
    latch_candidate(0);
    assert(!latched_ms);
    reset();
    puts("track lifecycle passed: fresh reload/refresh, canceled reads, metadata guards, cue reloads, queue coalescing and stale target cleanup");
    return 0;
}
