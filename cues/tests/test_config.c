/* SPDX-License-Identifier: GPL-3.0-only */
#include "cues.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static cues_config original, parsed;
static char json[128 * 1024 + 1], error[160];
static void rejected(const char *s)
{
    parsed = original;
    assert(!cues_config_parse(&parsed, s, strlen(s), error, sizeof error));
    assert(!memcmp(&original, &parsed, sizeof parsed));
    assert(error[0]);
}
int main(void)
{
    cues_config_default(&original);
    assert(!original.enabled && original.max_memory == 10 && original.hot_pads == 8);
    assert(cues_config_validate(&original, error, sizeof error));
    original.group_count = CUES_MAX_GROUPS;
    for (int i = 0; i < CUES_MAX_GROUPS; i++) {
        cues_group *g = &original.groups[i];
        cues_group_default(g, &original);
        g->rule_count = CUES_MAX_RULES;
        g->min_bpm = 80;
        g->max_bpm = 170;
        snprintf(g->name, sizeof g->name, "Group %d \"quoted\" / café", i);
        snprintf(g->comment, sizeof g->comment, "Newline\nTab\tBackslash\\");
        snprintf(g->artist, sizeof g->artist, "Artist");
        snprintf(g->key, sizeof g->key, "Am");
        for (int j = 0; j < CUES_MAX_RULES; j++) {
            cues_rule *r = &g->rules[j];
            cues_rule_default(r, g);
            r->anchor = j % 2;
            r->phrase_type = 1;
            r->position = r->anchor == CUES_ABSOLUTE ? j * 8 : 0;
            r->pad = j % 8 + 1;
            r->color = j % 9;
            r->cue_type = j % 2 ? AZ_CUE_MEMORY : AZ_CUE_HOT;
            if (r->cue_type == AZ_CUE_MEMORY)
                r->pad = 1;
            r->loop_mode = CUES_LOOP_FIXED;
            r->loop_length = 8;
        }
    }
    int n = cues_config_json(&original, json, sizeof json);
    assert(n > 0);
    assert(cues_config_parse(&parsed, json, (size_t)n, error, sizeof error));
    assert(!memcmp(&original, &parsed, sizeof original));
    assert(!cues_config_json(&original, json, 20));
    rejected("{}");
    rejected("{\"config\":{},\"groups\":[],\"groups\":[]}");
    rejected("{\"config\":{\"maxMemoryCues\":16},\"groups\":[]}");
    rejected("{\"config\":{\"minBpm\":200,\"maxBpm\":70},\"groups\":[]}");
    rejected("{\"config\":{\"silenceThreshold\":0.02},\"groups\":[]}");
    rejected("{\"config\":{},\"groups\":[{\"name\":\"x\",\"rules\":[{\"data\":{\"type\":\"hot\","
             "\"anchor\":{\"type\":\"stem\",\"stems\":[]}}}]}]}");
    rejected(
        "{\"config\":{},\"groups\":[{\"name\":\"x\",\"rules\":[{\"data\":{\"type\":\"hot\","
        "\"anchor\":{\"type\":\"phrase\",\"phrases\":[{\"type\":\"Intro\",\"stems\":[]}]}}}]}]}");
    rejected("{\"config\":{},\"groups\":[]} trailing");
    rejected("{\"config\":{\"minBpm\":NaN},\"groups\":[]}");
    rejected("{\"config\":{\"minBpm\":01},\"groups\":[]}");
    rejected("{\"config\":{},\"groups\":[{\"name\":\"\\u0000\"}]}");
    rejected("{\"config\":{},\"groups\":[{\"name\":\"\\ud800\"}]}");
    const char *unicode = "{\"config\":{},\"groups\":[{\"name\":\"\\ud83c\\udfb5 café\"}]}";
    assert(cues_config_parse(&parsed, unicode, strlen(unicode), error, sizeof error));
    assert(!strcmp(parsed.groups[0].name, "🎵 café"));
    const char *source =
        "{\"config\":{\"maxMemoryCues\":10,\"hotCuePads\":8,\"cuePreference\":\"Hot "
        "Cue\",\"quantization\":\"Beat\",\"measuresIn\":\"Bars\"},\"groups\":[{\"name\":\"Mix\","
        "\"quantization\":\"Beat\",\"bpmRange\":{\"min\":null,\"max\":null},\"rules\":[{\"data\":{"
        "\"type\":\"memory\",\"anchor\":{\"type\":\"phrase\",\"phrases\":[{\"anchor\":\"start\","
        "\"type\":\"Breakdown\",\"index\":\"All\"}]},\"loop\":{\"unit\":\"to end of "
        "phrase\"}},\"sentence\":\"display only\"}]}]}";
    assert(cues_config_parse(&parsed, source, strlen(source), error, sizeof error));
    assert(parsed.groups[0].rules[0].phrase_type == 3 &&
           parsed.groups[0].rules[0].loop_mode == CUES_LOOP_TO_END);
    /* Deterministic malformed-input sweep checks atomic rejection and token bounds. */
    for (unsigned i = 0; i < 1000; i++) {
        char garbage[128];
        size_t len = i % sizeof garbage;
        for (size_t j = 0; j < len; j++)
            garbage[j] = (char)(32 + (i * 37 + j * 71) % 95);
        parsed = original;
        if (!cues_config_parse(&parsed, garbage, len, error, sizeof error))
            assert(!memcmp(&parsed, &original, sizeof parsed));
    }
    puts("Config passed: full bounded round trip, reference JSON, Unicode, validation, atomic "
         "rejection and malformed input");
    return 0;
}
