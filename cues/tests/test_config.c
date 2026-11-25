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
    assert(!original.consolidate_phrases);
    original.consolidate_phrases = 1;
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
            r->phrase_index = r->anchor == CUES_PHRASE ? 1 : 0;
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
    assert(strstr(json, "\"sentence\":\"Create a Hot Cue on pad A, at the start of the 1st Intro phrase"));
    assert(cues_config_parse(&parsed, json, (size_t)n, error, sizeof error));
    assert(!memcmp(&original, &parsed, sizeof original));
    int compact = cues_config_json(&original, json, (size_t)n);
    assert(compact > 0 && compact < n && !strstr(json, "\"sentence\":"));
    assert(cues_config_parse(&parsed, json, (size_t)compact, error, sizeof error));
    assert(!memcmp(&original, &parsed, sizeof original));
    assert(!cues_config_json(&original, json, 20));
    rejected("{}");
    rejected("{\"config\":{},\"groups\":[],\"groups\":[]}");
    rejected("{\"config\":{\"maxMemoryCues\":16},\"groups\":[]}");
    rejected("{\"config\":{\"consolidatePhrases\":1},\"groups\":[]}");
    rejected("{\"config\":{\"minBpm\":200,\"maxBpm\":70},\"groups\":[]}");
    rejected("{\"config\":{\"silenceThreshold\":0.02},\"groups\":[]}");
    rejected(
        "{\"config\":{},\"groups\":[{\"name\":\"x\",\"rules\":[{\"data\":{\"type\":\"hot\","
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
    const char *exponents = "{\"config\":{\"minBpm\":1e2,\"maxBpm\":4E+2,\"minDistance\":1e-2},\"groups\":[]}";
    assert(cues_config_parse(&parsed, exponents, strlen(exponents), error, sizeof error));
    assert(parsed.min_bpm == 100 && parsed.max_bpm == 400);
    const char *scalar_boundaries = "{\"config\":{},\"groups\":[{\"name\":\"\\u007f\\u0080\\u07ff\\u0800\\uffff\"}]}";
    assert(cues_config_parse(&parsed, scalar_boundaries, strlen(scalar_boundaries), error, sizeof error));
    assert(!strcmp(parsed.groups[0].name, "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf"));
    rejected("{\"config\":{\"minBpm\":-},\"groups\":[]}");
    rejected("{\"config\":{\"minBpm\":1.},\"groups\":[]}");
    rejected("{\"config\":{\"minBpm\":1e},\"groups\":[]}");
    rejected("{\"config\":{\"minBpm\":1e+},\"groups\":[]}");
    rejected("{\"config\":{\"minBpm\":1e999},\"groups\":[]}");
    rejected("{\"config\":{},\"groups\":[{\"name\":\"\\udc00\"}]}");
    rejected("{\"config\":{},\"groups\":[{\"name\":\"\\ud800\\u0041\"}]}");
    rejected("{\"config\":{},\"groups\":[{\"name\":\"\xc0\x80\"}]}");
    rejected("{\"config\":{},\"groups\":[{\"name\":\"\xe0\x80\x80\"}]}");
    rejected("{\"config\":{},\"groups\":[{\"name\":\"\xf4\x90\x80\x80\"}]}");
    rejected("{\"config\":{},\"groups\":[{\"name\":\"\xe2\x82\"}]}");
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
    const char *automatic =
        "{\"config\":{},\"groups\":[{\"name\":\"All\",\"rules\":[{\"data\":{"
        "\"pad\":\"*\",\"color\":\"By pad\",\"anchor\":{\"type\":\"phrase\",\"phrases\":[{\"type\":\"Chorus/Drop\","
        "\"index\":\"All\"}]},\"type\":\"hot\"}}]}]}";
    assert(cues_config_parse(&parsed, automatic, strlen(automatic), error, sizeof error));
    assert(!parsed.consolidate_phrases);
    assert(cues_rule_auto_pad(&parsed.groups[0].rules[0]));
    assert(parsed.groups[0].rules[0].color == CUES_COLOR_BY_PAD);
    n = cues_config_json(&parsed, json, sizeof json);
    assert(n > 0 && strstr(json, "\"pad\":\"*\"") && strstr(json, "\"color\":\"By pad\""));
    cues_config round_trip = parsed;
    assert(cues_config_parse(&parsed, json, (size_t)n, error, sizeof error));
    assert(!memcmp(&parsed, &round_trip, sizeof parsed));
    rejected(
        "{\"config\":{},\"groups\":[{\"rules\":[{\"data\":{\"type\":\"hot\",\"pad\":\"*\","
        "\"anchor\":{\"type\":\"absolute\",\"absolute\":{\"value\":0,\"unit\":\"seconds\"}}}}]}]}");
    rejected(
        "{\"config\":{},\"groups\":[{\"rules\":[{\"data\":{\"type\":\"hot\",\"pad\":\"*\","
        "\"anchor\":{\"type\":\"phrase\",\"phrases\":[{\"type\":\"Intro\",\"index\":\"1\"}]}}}]}]}");
    rejected(
        "{\"config\":{},\"groups\":[{\"rules\":[{\"data\":{\"type\":\"memory\",\"pad\":\"*\","
        "\"anchor\":{\"type\":\"phrase\",\"phrases\":[{\"type\":\"Intro\",\"index\":\"All\"}]}}}]}]}");
    rejected(
        "{\"config\":{},\"groups\":[{\"rules\":[{\"data\":{\"type\":\"memory\",\"color\":\"By pad\","
        "\"anchor\":{\"type\":\"absolute\",\"absolute\":{\"value\":0,\"unit\":\"seconds\"}}}}]}]}");
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
    puts(
        "Config passed: full bounded round trip, reference JSON, Unicode, validation, atomic "
        "rejection and malformed input");
    return 0;
}
