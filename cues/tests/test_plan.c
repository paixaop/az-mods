/* SPDX-License-Identifier: GPL-3.0-only */
#include "cues.h"
#include "azmod-timestamps.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static cues_config config;
static az_track_snapshot track;
static az_native_cue_snapshot existing;
static az_beat beats[65];
static az_phrase phrases[5];
static cues_plan plan;
static const uint32_t rate = 48000;

static void at(double first, double second)
{
    int order = compare_timestamps(first, second, 1, rate, 1);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
}
static cues_rule *rule(int index, int anchor, int type, int unit, double position)
{
    cues_rule *r = &config.groups[0].rules[index];
    *r = (cues_rule){.enabled = 1, .anchor = anchor, .cue_type = type, .pad = index % 8 + 1,
                     .unit = unit, .offset_unit = CUES_SECONDS, .loop_unit = CUES_BEATS,
                     .position = position, .color = 2};
    if (config.groups[0].rule_count < index + 1) config.groups[0].rule_count = index + 1;
    return r;
}
static void setup(void)
{
    memset(&config, 0, sizeof config);
    config.enabled = 1;
    config.max_memory = 10;
    config.hot_pads = 8;
    config.quantization = CUES_QUANT_OFF;
    config.cue_preference = AZ_CUE_HOT;
    config.min_bpm = 70;
    config.max_bpm = 300;
    config.group_count = 1;
    config.groups[0].enabled = 1;
    config.groups[0].measures_in = CUES_BARS;
    memset(&track, 0, sizeof track);
    track.size = sizeof track;
    track.version = AZ_DATA_VERSION;
    track.metadata_info.availability = AZ_AVAIL_READY;
    track.grid_info.availability = AZ_AVAIL_READY;
    track.phrases_info.availability = AZ_AVAIL_READY;
    track.metadata.valid = AZ_META_TEMPO | AZ_META_ARTIST | AZ_META_KEY | AZ_META_DURATION;
    track.metadata.original_bpm = 120;
    track.metadata.duration_s = 40;
    strcpy(track.metadata.artist, "Artist Test");
    strcpy(track.metadata.key, "8A");
    for (unsigned i = 0; i < 65; ++i) beats[i] = (az_beat){.time_s = i * .5, .bpm = 120, .beat_in_bar = i % 4 + 1};
    track.beat_count = 65;
    track.beats = beats;
    phrases[0] = (az_phrase){.start_beat = 0, .end_beat = 8, .raw_mood = 1, .raw_kind = 1};
    phrases[1] = (az_phrase){.start_beat = 8, .end_beat = 16, .raw_mood = 1, .raw_kind = 1};
    phrases[2] = (az_phrase){.start_beat = 16, .end_beat = 24, .raw_mood = 1, .raw_kind = 5};
    phrases[3] = (az_phrase){.start_beat = 24, .end_beat = 32, .raw_mood = 2, .raw_kind = 2};
    phrases[4] = (az_phrase){.start_beat = 56, .end_beat = 65, .raw_mood = 1, .raw_kind = 6};
    track.phrase_count = 5;
    track.phrases = phrases;
    existing = (az_native_cue_snapshot){.size = sizeof existing, .version = AZ_DATA_VERSION,
        .native_load_generation = 1, .revision = 1};
}
static void build(void)
{
    int rc = cues_build_plan(&config, &track, &existing, rate, &plan);
    assert(rc == AZ_OK);
}
static void add_existing(int type, int pad, double start)
{
    assert(existing.count < AZ_NATIVE_CUE_LIMIT);
    existing.entries[existing.count++] = (az_cue_entry){.classification = type, .slot = pad,
                                                       .type = AZ_CUE_POINT, .valid = AZ_CUE_START, .start_s = start};
}
static void absolute_and_metadata(void)
{
    setup();
    cues_rule *r = rule(0, CUES_ABSOLUTE, AZ_CUE_HOT, CUES_SECONDS, 0);
    strcpy(r->name, "Start"); strcpy(r->comment, "{phrase}");
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 0); assert(!strcmp(plan.points[0].comment, "Start: Absolute"));
    assert(plan.points[0].slot == 1 && plan.points[0].rgb == cues_colors[2]);
    strcpy(config.groups[0].artist, "TEST"); strcpy(config.groups[0].key, "8a");
    build(); assert(plan.count == 1);
    track.metadata.valid &= ~AZ_META_ARTIST;
    build(); assert(plan.count == 0);
    track.metadata.valid |= AZ_META_ARTIST;
    track.metadata.original_bpm = 70;
    build(); assert(plan.count == 1);
    track.metadata.original_bpm = 300;
    build(); assert(plan.count == 1);
    track.metadata.original_bpm = 301;
    build(); assert(plan.count == 0);
    config.groups[0].max_bpm = 310;
    build(); assert(plan.count == 1);
    config.enabled = 0;
    build(); assert(plan.count == 0);

    setup();
    rule(0, CUES_ABSOLUTE, AZ_CUE_MEMORY, CUES_SECONDS, 3.25);
    track.grid_info.availability = AZ_AVAIL_UNSUPPORTED; track.beats = NULL; track.beat_count = 0;
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 3.25);
    config.quantization = CUES_QUANT_BEAT;
    build(); assert(plan.count == 0 && plan.skipped == 1);
    assert(cues_build_plan(&config, &track, &existing, 0, &plan) == AZ_INVALID);
    config.enabled = 1;
    track.metadata.valid &= ~AZ_META_DURATION;
    assert(cues_build_plan(&config, &track, &existing, rate, &plan) == AZ_PENDING);
}
static void phrase_rules(void)
{
    setup();
    cues_rule *r = rule(0, CUES_PHRASE, AZ_CUE_MEMORY, CUES_SECONDS, 0);
    r->phrase_type = 1;
    strcpy(r->name, "{length} {phrase}");
    build(); assert(plan.count == 2); at(plan.points[0].start_s, 0); at(plan.points[1].start_s, 4);
    assert(!strcmp(plan.points[0].comment, "2 Intro"));
    r->offset_unit = CUES_BARS;
    for (unsigned i = 0; i < 65; ++i) beats[i].beat_in_bar = 0;
    build(); assert(plan.count == 2); /* Zero bar displacement needs no inferred meter. */
    r->offset = 1;
    build(); assert(plan.count == 0); /* A real bar offset requires measured downbeats. */
    r->offset = 0; r->offset_unit = CUES_SECONDS;
    for (unsigned i = 0; i < 65; ++i) beats[i].beat_in_bar = i % 4 + 1;
    r->phrase_index = 2; r->phrase_point = CUES_MIDDLE; r->offset = -.5;
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 5.5);
    r->phrase_point = CUES_END; r->offset = 0;
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 8);
    r->phrase_type = 6; r->phrase_index = 0;
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 16);
    r->phrase_type = 5;
    build(); assert(plan.count == 0 && plan.skipped == 1); /* missing exact terminal beat */
    r->phrase_type = 0; r->phrase_point = CUES_START;
    phrases[1].raw_mood = 0; phrases[1].raw_kind = 99;
    build(); assert(plan.count == 4); /* unknown phrase still matches Any */
    r->phrase_type = 1;
    build(); assert(plan.count == 1);
    phrases[1].raw_mood = 1; phrases[1].raw_kind = 1;
    phrases[1].start_beat = 7; /* overlap must not produce a second cue */
    build(); assert(plan.count == 1 && plan.skipped == 1);
}
static void variable_grid_and_loops(void)
{
    setup();
    for (unsigned i = 0; i < 65; ++i) {
        beats[i].time_s = i < 12 ? i * .5 : 6 + (i - 12) * .75;
        beats[i].bpm = i < 12 ? 120 : 80;
        beats[i].beat_in_bar = i % 3 + 1; /* measured three-beat bars */
    }
    track.metadata.duration_s = 50;
    cues_rule *r = rule(0, CUES_ABSOLUTE, AZ_CUE_MEMORY, CUES_BARS, 4.5);
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 7.125);
    r->unit = CUES_BEATS; r->position = 12.5;
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 6.375);
    r->loop_mode = CUES_LOOP_FIXED; r->loop_unit = CUES_BARS; r->loop_length = 1;
    build(); assert(plan.count == 1 && plan.points[0].type == AZ_CUE_LOOP); at(plan.points[0].end_s, 8.625);
    r->loop_unit = CUES_BEATS; r->loop_length = 2;
    build(); at(plan.points[0].end_s, 7.875);
    r->anchor = CUES_PHRASE; r->phrase_type = 4; r->phrase_point = CUES_MIDDLE;
    r->offset_unit = CUES_BARS; r->offset = -1;
    r->loop_mode = CUES_LOOP_TO_END;
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 9.75); at(plan.points[0].end_s, 15);
    r->offset = 0; r->loop_mode = CUES_LOOP_FROM_START;
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 9); at(plan.points[0].end_s, 12);
    r->phrase_point = CUES_START;
    build(); assert(plan.count == 0); /* empty loop */
    r->anchor = CUES_ABSOLUTE; r->loop_mode = CUES_LOOP_TO_END;
    build(); assert(plan.count == 0);
    r->loop_mode = CUES_NO_LOOP; r->unit = CUES_BARS;
    for (unsigned i = 0; i < 65; ++i) beats[i].beat_in_bar = 0;
    build(); assert(plan.count == 0); /* no meter invented */
}
static void quantization(void)
{
    setup();
    cues_rule *r = rule(0, CUES_ABSOLUTE, AZ_CUE_MEMORY, CUES_SECONDS, 1.25);
    config.quantization = CUES_QUANT_BEAT;
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 1); /* ties earlier */
    r->position = 1.3;
    build(); at(plan.points[0].start_s, 1.5);
    config.groups[0].quantization = CUES_QUANT_BAR;
    build(); at(plan.points[0].start_s, 2);
    config.groups[0].quantization = CUES_QUANT_OFF;
    build(); at(plan.points[0].start_s, 1.3);
    r->position = -1;
    build(); assert(plan.count == 0);
    r->position = 40;
    build(); assert(plan.count == 0);
    r->position = NAN;
    assert(cues_build_plan(&config, &track, &existing, rate, &plan) == AZ_INVALID);
    r->position = 1;
    beats[2].time_s = beats[1].time_s;
    config.quantization = CUES_QUANT_BEAT; config.groups[0].quantization = CUES_INHERIT;
    build(); assert(plan.count == 0);
}
static void conflicts_and_limits(void)
{
    setup();
    rule(0, CUES_ABSOLUTE, AZ_CUE_HOT, CUES_SECONDS, 0);
    add_existing(AZ_CUE_HOT, 1, 20);
    build(); assert(plan.count == 0);
    existing.count = 0;
    add_existing(AZ_CUE_MEMORY, 0, 0);
    build(); assert(plan.count == 0);
    existing.entries[0].start_s = 2;
    config.min_distance = 3;
    build(); assert(plan.count == 0);
    config.min_distance = 2;
    build(); assert(plan.count == 1);

    setup();
    rule(0, CUES_ABSOLUTE, AZ_CUE_MEMORY, CUES_SECONDS, 4);
    rule(1, CUES_ABSOLUTE, AZ_CUE_HOT, CUES_SECONDS, 4);
    build(); assert(plan.count == 1 && plan.points[0].classification == AZ_CUE_HOT);
    config.cue_preference = AZ_CUE_MEMORY;
    build(); assert(plan.count == 1 && plan.points[0].classification == AZ_CUE_MEMORY);

    setup();
    for (int i = 0; i < 5; ++i) rule(i, CUES_ABSOLUTE, AZ_CUE_MEMORY, CUES_SECONDS, i * 2.);
    for (int i = 0; i < 8; ++i) add_existing(AZ_CUE_MEMORY, 0, 20 + i);
    build(); assert(plan.count == 2); at(plan.points[1].start_s, 2);
    config.max_memory = 8;
    build(); assert(plan.count == 0);

    setup();
    rule(0, CUES_ABSOLUTE, AZ_CUE_HOT, CUES_SECONDS, 2);
    rule(1, CUES_ABSOLUTE, AZ_CUE_HOT, CUES_SECONDS, 4)->pad = 1;
    build(); assert(plan.count == 1); at(plan.points[0].start_s, 2);
    rule(2, CUES_ABSOLUTE, AZ_CUE_HOT, CUES_SECONDS, 6)->pad = 8;
    config.hot_pads = 2;
    build(); assert(plan.count == 1);

    setup();
    rule(0, CUES_ABSOLUTE, AZ_CUE_MEMORY, CUES_SECONDS, 1);
    config.group_count = 2;
    config.groups[1] = config.groups[0];
    config.groups[1].rules[0].position = 3;
    build(); assert(plan.count == 2);
    config.groups[1].enabled = 0;
    build(); assert(plan.count == 1);
    add_existing(AZ_CUE_MEMORY, 0, 20);
    existing.entries[0].valid = 0;
    assert(cues_build_plan(&config, &track, &existing, rate, &plan) == AZ_STALE);
}
static void utf8_labels(void)
{
    setup();
    cues_rule *r = rule(0, CUES_ABSOLUTE, AZ_CUE_MEMORY, CUES_SECONDS, 1);
    strcpy(r->name, "\xc3\x89" "t\xc3\xa9");
    memset(r->comment, 'x', sizeof r->comment - 1);
    r->comment[sizeof r->comment - 1] = 0;
    build(); assert(plan.count == 1 && strlen(plan.points[0].comment) == 127);
    assert(!memcmp(plan.points[0].comment, r->name, strlen(r->name)));
}
static void maximum_analysis(void)
{
    static az_beat large_grid[32768];
    static az_phrase large_phrases[2048];
    setup();
    for (unsigned i = 0; i < 32768; ++i)
        large_grid[i] = (az_beat){.time_s = i * .0625, .bpm = 120, .beat_in_bar = i % 4 + 1};
    for (unsigned i = 0; i < 2048; ++i)
        large_phrases[i] = (az_phrase){.start_beat = i * 16, .end_beat = i * 16 + 12,
                                      .raw_mood = 1, .raw_kind = 1};
    track.beats = large_grid; track.beat_count = 32768;
    track.phrases = large_phrases; track.phrase_count = 2048;
    track.metadata.duration_s = 2500;
    config.min_distance = 3600;
    add_existing(AZ_CUE_MEMORY, 0, 1000);
    for (int j = 0; j < CUES_MAX_RULES; ++j) {
        cues_rule *r = rule(j, CUES_PHRASE, AZ_CUE_MEMORY, CUES_BARS, 0);
        r->phrase_point = CUES_MIDDLE; r->offset_unit = CUES_BARS; r->offset = -.25;
        r->loop_mode = CUES_LOOP_FIXED; r->loop_unit = CUES_BARS; r->loop_length = 1;
    }
    config.group_count = CUES_MAX_GROUPS;
    for (int i = 1; i < CUES_MAX_GROUPS; ++i) config.groups[i] = config.groups[0];
    build();
    assert(plan.count == 0 && plan.skipped == CUES_MAX_GROUPS * CUES_MAX_RULES * 2048);
}
int main(void)
{
    absolute_and_metadata(); phrase_rules(); variable_grid_and_loops(); quantization(); conflicts_and_limits(); utf8_labels(); maximum_analysis();
    puts("Cue planning passed: source clocks, phrases, variable tempo/meter, loops, quantization, filters, preservation, limits and UTF-8");
    return 0;
}
