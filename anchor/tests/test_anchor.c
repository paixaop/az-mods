/* SPDX-License-Identifier: GPL-3.0-only */
/* Target selection and transition arithmetic on a 48 kHz fixture track. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "azmod-timestamps.h"
#include "guide.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define TRACK_RATE 48000

static int same_time(uint64_t first, uint64_t second, unsigned rate)
{
    int order = compare_timestamps(first, second, 1000, rate, 1);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL;
}

static an_deck_t deck(uint32_t pos, int playing)
{
    an_deck_t d;
    memset(&d, 0, sizeof d);
    d.loaded = 1;
    d.playing = playing;
    d.pos_ms = pos;
    d.bpm = 120.0;
    d.rate = 1.0;
    d.sample_rate = TRACK_RATE;
    return d;
}

int main(void)
{
    an_beat_t beats[40];
    for (unsigned i = 0; i < 40; i++) beats[i] = (an_beat_t){i * 500, 12000, i % 4 + 1};
    const an_phrase_t phrases[] = {
        {0, 4000, AN_CAT_INTRO, 0},
        {4000, 8000, AN_CAT_BUILD, 1},
        {8000, 12000, AN_CAT_CHORUS, 1},
        {12000, 16000, AN_CAT_CHORUS, 1},
        {16000, 19500, AN_CAT_OUTRO, 1},
    };
    const an_cue_t cues[] = {{.time_ms = 10000, .slot = 1}};
    const an_analysis_t a = {
        .beats = beats, .nbeats = 40, .phrases = phrases, .nphrases = 5, .cues = cues, .ncues = 1,
    };

    an_target_opts_t o = { AN_RULE_PHRASE, AN_CAT_CHORUS, AN_POINT_END, 1, 1, 1 };
    an_cand_t c;
    an_group_t groups[16];
    CHECK(an_phrase_groups(&a, 1, groups, 16) == 4); /* the two choruses join */
    CHECK(an_phrase_groups(&a, 0, groups, 16) == 5);
    CHECK(an_phrase_groups(&a, 1, groups, 16) == 4 && same_time(groups[2].start_ms, 8000, TRACK_RATE) &&
          same_time(groups[2].end_ms, 16000, TRACK_RATE));
    CHECK(an_auto_target(&a, 9000, &o, &c) == 0);
    CHECK(same_time(c.ms, 16000, TRACK_RATE));
    CHECK(strcmp(c.label, "CHORUS END") == 0);
    o.consolidate = 0;
    CHECK(an_auto_target(&a, 9000, &o, &c) == 0 && same_time(c.ms, 12000, TRACK_RATE));
    o.consolidate = 1;
    o.phrase_point = AN_POINT_START;
    CHECK(an_auto_target(&a, 1000, &o, &c) == 0 && same_time(c.ms, 8000, TRACK_RATE));
    CHECK(strcmp(c.label, "CHORUS START") == 0);
    o.rule = AN_RULE_HOTCUE;
    o.hotcue_any = 1;
    CHECK(an_auto_target(&a, 1000, &o, &c) == 0 && same_time(c.ms, 10000, TRACK_RATE));
    CHECK(strcmp(c.label, "HC1") == 0);
    o.hotcue_any = 0;
    o.hotcue_slot = 1;
    CHECK(an_auto_target(&a, 1000, &o, &c) == 0 && same_time(c.ms, 10000, TRACK_RATE));
    o.hotcue_slot = 2;
    CHECK(an_auto_target(&a, 1000, &o, &c) != 0);
    CHECK(an_find_drop(&a, 0, &c) == 0 && same_time(c.ms, 8000, TRACK_RATE));

    o.rule = AN_RULE_PHRASE;
    o.phrase_cat = -1; /* the default: the section playing now */
    o.phrase_point = AN_POINT_END;
    o.consolidate = 1;
    CHECK(an_auto_target(&a, 9000, &o, &c) == 0 && same_time(c.ms, 16000, TRACK_RATE));
    o.consolidate = 0;
    CHECK(an_auto_target(&a, 9000, &o, &c) == 0 && same_time(c.ms, 12000, TRACK_RATE));
    o.phrase_cat = AN_CAT_INTRO; /* no intro start ahead: fall back to the next phrase start */
    o.phrase_point = AN_POINT_START;
    o.consolidate = 1;
    CHECK(an_auto_target(&a, 1000, &o, &c) == 0 && same_time(c.ms, 4000, TRACK_RATE));
    CHECK(strcmp(c.label, "BUILD START") == 0);
    o.phrase_cat = AN_CAT_CHORUS;
    o.phrase_point = AN_POINT_END;
    an_cand_t cands[14];
    int cn = an_candidates(&a, 1000, TRACK_RATE, &o, cands, 14);
    CHECK(cn >= 4);
    CHECK(cands[0].ms <= cands[cn - 1].ms);
    int saw_cue = 0, saw_chorus_end = 0;
    for (int i = 0; i < cn; i++) {
        if (same_time(cands[i].ms, 10000, TRACK_RATE)) saw_cue = 1;
        if (same_time(cands[i].ms, 16000, TRACK_RATE)) saw_chorus_end = 1;
    }
    CHECK(saw_cue && saw_chorus_end);

    /* A target 16 s; B paused at 0, B drop 8 s: play at 8 s. */
    an_deck_t A = deck(1000, 1), B = deck(0, 0);
    an_loop_t loop;
    memset(&loop, 0, sizeof loop);
    an_plan_t p;
    const an_cand_t destination = {.ms = 8000, .label = "CHORUS"};
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 0, &p);
    CHECK(p.state == AN_ST_SINGLE);
    CHECK(p.action == 1);
    CHECK(same_time(p.action_wall_ms, 7000, 1000));

    /* Countdown -> exact PLAY NOW -> missed. Never cue before the deadline; retain the
     * cue briefly after it so the next status callback can paint it. */
    A.pos_ms = 7900;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 6900, &p);
    CHECK(p.state == AN_ST_SINGLE && !p.play_now && p.bars_action > 0);
    A.pos_ms = 8000;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 7000, &p);
    CHECK(p.state == AN_ST_SINGLE && p.play_now && p.accent == 4);
    A.pos_ms = 8100;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 7100, &p);
    CHECK(p.state == AN_ST_SINGLE && p.play_now);
    A.pos_ms = 8200;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 7200, &p);
    CHECK(p.state == AN_ST_MISSED && !p.play_now);
    A = deck(1000, 1);
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, UINT64_C(5000000000), &p);
    CHECK(same_time(p.action_wall_ms, UINT64_C(5000007000), 1000));

    /* 4-bar loop out at the drop: the start window is the same 7 s, with one loop. */
    loop.valid = 1;
    loop.in_ms = 0;
    loop.out_ms = 8000;
    loop.len_ms = 8000;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 0, &p);
    CHECK(p.state == AN_ST_START);
    CHECK(p.loops == 1);

    /* A paused loop must not jump to its next start slot before showing PLAY NOW. */
    A.pos_ms = 7900;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 6900, &p);
    CHECK(p.state == AN_ST_START && !p.play_now && p.bars_action > 0);
    A.pos_ms = 8000;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 7000, &p);
    CHECK(p.state == AN_ST_START && p.play_now);
    A.pos_ms = 8100;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 7100, &p);
    CHECK(p.state == AN_ST_START && p.play_now);
    A.pos_ms = 8200;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 7200, &p);
    CHECK(p.state == AN_ST_MISSED && !p.play_now);
    A = deck(1000, 1);

    /* B looping at 1 s: the aligned pass ends at 15 s, so its window opens at 7 s. */
    B = deck(1000, 1);
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 0, &p);
    CHECK(p.state == AN_ST_WAIT);
    CHECK(p.window_open == 0);
    CHECK(fabs(p.bars_action - 3.5) < 0.6); /* 7 s at 120 BPM = 3.5 bars */

    /* At 7 s the pass that ends at 14 s is the aligned one, and its window is open. */
    A.pos_ms = 1000;
    B = deck(1000, 1);
    an_plan_target(&A, &B, &loop, 8000, "CHORUS END", &destination, 7000, &p);
    CHECK(p.state == AN_ST_OPEN);
    CHECK(p.window_open == 1);

    /* EXIT NOW must not appear in the preceding pass, even 25 ms before loop-out. */
    A = deck(7975, 1);
    B = deck(7975, 1);
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 0, &p);
    CHECK(p.state == AN_ST_WAIT && !p.window_open && p.bars_action > 0);
    A = deck(8000, 1);
    B = deck(0, 1);
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 25, &p);
    CHECK(p.state == AN_ST_OPEN && p.window_open);

    /* A loop after the drop cannot be used. */
    loop.out_ms = 20000;
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 0, &p);
    CHECK(p.state == AN_ST_OFF);

    /* No time left for the single start: a missed target. */
    memset(&loop, 0, sizeof loop);
    A = deck(9000, 1);
    B = deck(0, 0);
    an_plan_target(&A, &B, &loop, 12000, "CHORUS END", &destination, 0, &p);
    CHECK(p.state == AN_ST_MISSED);

    /* The loop was released and the drop is being waited for. */
    memset(&loop, 0, sizeof loop);
    loop.released = 1;
    A = deck(1000, 1);
    B = deck(7600, 1);
    an_plan_target(&A, &B, &loop, 16000, "CHORUS END", &destination, 0, &p);
    CHECK(p.state == AN_ST_RELEASED);

    printf("all anchor host tests passed\n");
    return 0;
}
