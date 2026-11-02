/* Regression tests for real sampled playheads, not fixed loop-length assumptions. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "anchor.c"

static az_deck_snapshot native;
static int native_enabled;
static int snapshot(int d, az_deck_snapshot *out)
{
    if (!native_enabled) return -1;
    *out = native; out->deck = (unsigned)d; return AZ_OK;
}
static void reset(void)
{
    static az_core_v1 core;
    memset(&core,0,sizeof core); core.size=sizeof core; core.deck_snapshot=snapshot;
    C=&core; prepared_reader_ok=0; memset(prepared_loop,0,sizeof prepared_loop); memset(rt,0,sizeof rt); memset(&native,0,sizeof native); native_enabled=0;
}
static void sample(double pos, uint64_t now, int playing)
{
    if(native_enabled)native.observed_ns=now*1000000u;
    az_deck_info info = {.size=sizeof info,.loaded=1,.playing=playing,.pos_s=pos/1000,.content_id=42};
    update_runtime(1,&info,now);
}
static void loop(unsigned length, double rate, unsigned in)
{
    reset();
    const unsigned delta[] = {73,137,83,107,91,119};
    uint64_t now=0;
    double duration=length*2.4/rate;
    int saw_candidate=0;
    unsigned samples=0;
    while (now < duration) {
        double pos=in+fmod(now*rate,length);
        sample(pos,now,1);
        if(rt[1].candidate && !rt[1].confirmed)saw_candidate=1;
        now+=delta[samples++%6];
    }
    assert(saw_candidate && rt[1].confirmed);
    assert(fabs((double)rt[1].len_ms-length)<=3);
    assert(rt[1].in_low<=in && rt[1].in_high>=in);
    /* The previous implementation lost up to one sample of length on every wrap. */
    assert(rt[1].out_ms>rt[1].in_ms && !rt[1].released);
    double exit_pos=rt[1].last_pos;
    uint64_t exit_time=rt[1].last_ms;
    unsigned out=rt[1].out_ms;
    while(exit_pos < out+350) { exit_pos+=rate*100; exit_time+=100; sample(exit_pos,exit_time,1); }
    assert(!rt[1].confirmed && rt[1].released);
}
int main(void)
{
    const unsigned lengths[]={500,1000,2000,8000,16000,64000};
    for(unsigned i=0;i<sizeof lengths/sizeof lengths[0];i++) {
        loop(lengths[i],1.0,0);
        loop(lengths[i],1.12,16000);
        loop(lengths[i],.85,32000);
    }
    reset();
    sample(10000,0,1); sample(10100,100,1); sample(2100,200,1); /* one backward seek */
    assert(rt[1].candidate && !rt[1].confirmed);
    sample(2200,300,1); sample(2300,400,0);
    assert(!rt[1].candidate && !rt[1].confirmed);
    reset(); sample(0,0,1); sample(100,100,1);
    assert(rt[1].have_last && rt[1].rate_ok); /* track time zero is valid */
    sample(200,2000,1); assert(!rt[1].confirmed); /* long sample outage */
    sample(NAN,2100,1); assert(!rt[1].have_last);
    reset();
    for(unsigned t=0;t<20000;t+=100)sample(16000+t%8000,t,1);
    assert(rt[1].confirmed && rt[1].len_ms==8000);
    sample(16000,20000,1); /* halve the active loop: no old length remains confirmed */
    assert(!rt[1].confirmed && rt[1].candidate);
    for(unsigned t=20100;t<=24000;t+=100)sample(16000+t%4000,t,1);
    assert(rt[1].confirmed && rt[1].len_ms==4000);
    /* Feed the observed loop through the real integration and deadline arithmetic. */
    an_beat_t beats[161];
    for(int i=0;i<161;i++)beats[i]=(an_beat_t){.time_ms=(uint32_t)i*500,.tempo=12000,.beat_in_bar=(uint8_t)(i%4+1)};
    an_phrase_t phrases[2]={{.start_beat=1,.end_beat=65,.kind=2},{.start_beat=65,.end_beat=161,.kind=5}};
    an_anlz_t analysis={.valid=1,.has_grid=1,.has_phrases=1,.bpm=120,.mood=1,
                       .beats=beats,.nbeats=161,.phrases=phrases,.nphrases=2,.end_beat=161};
    deck_analysis[0]=deck_analysis[1]=analysis; status[0]=status[1]=ST_OK;
    az_deck_info info[4]={{.loaded=1,.playing=1,.pos_s=1,.bpm=120},
                         {.loaded=1,.playing=1,.pos_s=16,.bpm=120}};
    latched_ms=33000; latched_gen=deck_gen[0]; snprintf(latched_label,sizeof latched_label,"CHOSEN TARGET");
    an_target_opts_t opts={.rule=AN_RULE_PHRASE,.phrase_cat=-1};
    compute_plan(24000,info,0,1,&opts);
    assert(plan.state==AN_ST_WAIT && plan.looping && fabs(plan.bars_action-8)<.01);
    assert(plan.out_wall_ms==44000); /* 4s loop plus the 12s tail lands at wall time 56s */
    /* A native prepared loop must change the paused PLAY deadline before any wrap. */
    prepared_reader_ok=1;
    memset(rt,0,sizeof rt);
    prepared_loop[1].state=(an_loaded_loop_t){.available=1,.engaged=1,.in_ms=16000,.out_ms=24000};
    prepared_loop[1].seq=latest_seq[1];prepared_loop[1].observed_ms=24000;
    info[1].playing=0;info[1].pos_s=16;
    update_runtime(1,&info[1],24000);
    assert(rt[1].confirmed && rt[1].native_loop);
    compute_plan(24000,info,0,1,&opts);
    assert(plan.state==AN_ST_START && plan.loop_prepared && plan.loops>1);
    an_display_t ready;
    an_display_model(&plan,0,1,&ready);
    assert(strstr(ready.deck,"LOOP READY"));
    uint64_t loop_launch=plan.action_wall_ms;
    prepared_loop[1].state.engaged=0;prepared_loop[1].observed_ms=24100;
    update_runtime(1,&info[1],24100);compute_plan(24100,info,0,1,&opts);
    assert(plan.state==AN_ST_SINGLE && !plan.loop_prepared && plan.action_wall_ms>loop_launch+8000);
    /* Invalid/old native data cannot silently produce a no-loop countdown. */
    update_runtime(1,&info[1],26000);compute_plan(26000,info,0,1,&opts);
    assert(plan.notice==AN_NOTICE_LOOP_UNKNOWN && !plan.action && !plan.play_now);
    prepared_loop[1].state=(an_loaded_loop_t){.available=1,.engaged=1,.in_ms=16000,.out_ms=24000};
    prepared_loop[1].observed_ms=26000;prepared_loop[1].seq=latest_seq[1];
    info[1].pos_s=8;update_runtime(1,&info[1],26000);
    assert(rt[1].confirmed); /* B may approach an already prepared loop from before IN. */
    latest_seq[1]++;info[1].playing=1;update_runtime(1,&info[1],26010);
    assert(rt[1].loop_pending && !rt[1].confirmed); /* prior-load native data must not qualify */
    memset(deck_analysis,0,sizeof deck_analysis); memset(status,0,sizeof status);
    reset();
    native_enabled=1;
    native.version=AZ_DATA_VERSION;
    native.valid=AZ_LIVE_LOADED|AZ_LIVE_POSITION|AZ_LIVE_TRACK;
    native.loaded=1; native.position_s=17; native.track.track_id=42;
    native.loop.valid=AZ_LOOP_DEFINED|AZ_LOOP_ENGAGED;
    native.loop.size=sizeof native.loop; native.loop.version=AZ_DATA_VERSION;
    native.loop.defined=native.loop.engaged=1; native.loop.in_s=16; native.loop.out_s=24;
    sample(17000,100,0); /* engaged native loop is available even while paused */
    assert(rt[1].native_loop && rt[1].confirmed && rt[1].len_ms==8000);
    native.loop.engaged=0; sample(17000,200,1);
    assert(!rt[1].confirmed && rt[1].released);
    native.loop.engaged=1; native.track.track_id=99; sample(17000,300,0);
    assert(!rt[1].confirmed); /* stale/wrong-track snapshot */
    native.track.track_id=42; native.loop.valid=0; sample(17000,400,0);
    assert(!rt[1].confirmed); /* a stored loop is not an active loop */
    puts("runtime passed: repeated wraps, true length, jitter/pitch, short/long loops, seeks, pause, zero and native validity");
}
