/* Destination intent survives transport changes; production picker and timing path. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "anchor.c"

static az_deck_info live[4];
static int info(int deck, az_deck_info *out) { *out = live[deck]; return 0; }
static void redraw(az_surface *surface) { (void)surface; }
static void show(az_surface *surface, int on) { (void)surface; (void)on; }

int main(void)
{
    static az_core_v1 core;
    core.size = sizeof core; core.deck_info = info;
    core.surface_redraw = redraw; core.surface_show = show;
    C = &core;
    cur_a = 0; cur_b = 1; deck_gen[0] = 2; deck_gen[1] = 4;
    an_beat_t beats[257];
    for (int i = 0; i < 257; i++) beats[i] = (an_beat_t){.time_ms = (unsigned)i * 500,
        .tempo = 12000, .beat_in_bar = (unsigned)i % 4 + 1};
    an_phrase_t phrases[] = {{.start_beat=1,.end_beat=65,.kind=2},
        {.start_beat=65,.end_beat=129,.kind=5}, {.start_beat=129,.end_beat=193,.kind=2},
        {.start_beat=193,.end_beat=257,.kind=5}};
    an_cue_t cues[] = {{.kind=AN_CUE_HOT,.slot=1,.time_ms=0},
        {.kind=AN_CUE_MEMORY,.time_ms=1000}, {.kind=AN_CUE_HOT,.slot=3,.time_ms=80000}};
    an_anlz_t analysis = {.valid=1,.has_grid=1,.has_phrases=1,.bpm=120,.mood=1,
        .beats=beats,.nbeats=257,.phrases=phrases,.nphrases=4,.cues=cues,.ncues=3,.end_beat=257};
    deck_analysis[0] = deck_analysis[1] = analysis; status[0] = status[1] = ST_OK;
    live[0] = (az_deck_info){.loaded=1,.playing=1,.pos_s=1,.bpm=120};
    live[1] = (az_deck_info){.loaded=1,.playing=0,.pos_s=16,.bpm=120};
    an_target_opts_t opts = {.rule=AN_RULE_PHRASE,.phrase_cat=-1};
    latched_ms = 65000; latched_gen = deck_gen[0];
    snprintf(latched_label,sizeof latched_label,"A DESTINATION");
    compute_plan(1000,live,0,1,&opts);
    assert(b_targets[1].valid && b_targets[1].target.ms == 32000);
    assert(plan.b_target_ms == 32000 && plan.action_wall_ms == 49000);
    live[1].pos_s = 4; /* jump backwards across a different phrase */
    compute_plan(1000,live,0,1,&opts);
    assert(plan.b_target_ms == 32000 && plan.action_wall_ms == 37000);
    live[1].pos_s = 70; /* moving ahead must not silently select the second chorus */
    compute_plan(1000,live,0,1,&opts);
    assert(plan.valid && plan.notice == AN_NOTICE_B_TARGET_PASSED && plan.b_target_ms == 32000);
    assert(b_targets[1].suggested.ms == 32000);
    live[1].pos_s = 16; live[1].playing = 1;
    rt[1].confirmed=rt[1].native_loop=1; rt[1].in_ms=16000; rt[1].out_ms=24000; rt[1].len_ms=8000;
    compute_plan(1000,live,0,1,&opts);
    uint64_t out = plan.out_wall_ms;
    assert(out && plan.b_target_ms == 32000);
    rt[1].in_ms=12000; rt[1].out_ms=20000; rt[1].len_ms=8000;
    live[1].pos_s=12;
    compute_plan(1000,live,0,1,&opts);
    assert(plan.b_target_ms == 32000 && plan.state == AN_ST_OFF);
    live[1].pos_s=16;
    compute_plan(1000,live,0,1,&opts);
    assert(plan.b_target_ms == 32000 && plan.out_wall_ms == out - 4000);
    rt[1].out_ms=36000; rt[1].len_ms=24000;
    compute_plan(1000,live,0,1,&opts);
    assert(plan.notice == AN_NOTICE_LOOP_AFTER_DROP && plan.b_target_ms == 32000);
    memset(rt,0,sizeof rt);

    /* B picker lists all phrase starts, independently of A settings/head position. */
    open_b_selector(0);
    assert(sel_n == 4 && sel_cand[0].ms == 0 && sel_cand[3].ms == 96000);
    latch_candidate(3);
    assert(!sel_open && b_targets[1].target.ms == 96000);
    cues_requested[1] = 1;
    open_b_selector(1);
    assert(sel_n == 2 && sel_cand[0].ms == 0 && sel_cand[1].ms == 80000);
    latch_candidate(0);
    assert(b_targets[1].valid && b_targets[1].target.ms == 0); /* zero is a real destination */
    open_b_selector(0); live[1].pos_s=43.217;
    set_b_here(); assert(b_targets[1].target.ms == 43217);
    assert(!strcmp(b_targets[1].target.label,"MARKED POINT"));
    /* Loop/pause/seek resets do not touch intent. */
    reset_runtime(1); assert(b_targets[1].target.ms == 43217);
    open_b_selector(0);
    invalidate_deck(1);
    assert(!b_targets[1].valid && !sel_open && !layout_ok);
    latch_candidate(2); set_b_here(); assert(!b_targets[1].valid);

    /* Set here remains useful with no B phrase/cue analysis. */
    cur_a=0;cur_b=1; status[0]=ST_OK;status[1]=ST_FAILED;
    sel_deck=1;sel_gen=deck_gen[1];live[1].pos_s=10;live[1].playing=0;
    set_b_here();live[1].pos_s=2;
    compute_plan(1000,live,0,1,&opts);
    assert(plan.state==AN_ST_SINGLE && plan.b_target_ms==10000);
    puts("B target passed: fixed intent across seeks/loops, independent phrase/Hot Cue/Set here, zero, reload/stale taps and missing B analysis");
}
