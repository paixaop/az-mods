/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CUES_H
#define CUES_H
#include "azmod-mod.h"
#define CUES_ID "cues"
#define CUES_MAX_GROUPS 8
#define CUES_MAX_RULES 16
#define CUES_MAX_PLAN 18
/* Values are persisted as JSON. Coordinates are zero-based, original-track
 * time. */
enum { CUES_PHRASE, CUES_ABSOLUTE };
enum { CUES_BARS, CUES_BEATS, CUES_SECONDS };
enum { CUES_INHERIT, CUES_QUANT_OFF, CUES_QUANT_BEAT, CUES_QUANT_BAR };
enum { CUES_START, CUES_MIDDLE, CUES_END };
enum { CUES_NO_LOOP, CUES_LOOP_FIXED, CUES_LOOP_FROM_START, CUES_LOOP_TO_END };
/* Cues phrase selectors: 0 any, 1 intro, 2 up/build, 3 down/break,
 * 4 chorus/drop, 5 outro, 6 verse, 7 bridge. */
typedef struct {
    int enabled, anchor, cue_type, pad, phrase_type, phrase_index, phrase_point;
    int unit, offset_unit, loop_mode, loop_unit, color;
    double position, offset, loop_length;
    char name[64], comment[128];
} cues_rule;
/* All phrase occurrences use the next free hot pad; pad is otherwise fixed. */
static inline int cues_rule_auto_pad(const cues_rule *rule)
{
    return rule->cue_type == AZ_CUE_HOT && rule->anchor == CUES_PHRASE &&
           rule->phrase_index == 0;
}
typedef struct {
    int enabled, quantization, measures_in, rule_count;
    double min_bpm, max_bpm; /* zero inherits global range */
    char name[64], comment[128], artist[64], key[32];
    cues_rule rules[CUES_MAX_RULES];
} cues_group;
typedef struct {
    int enabled, max_memory, hot_pads, quantization, measures_in,
        cue_preference;
    double min_bpm, max_bpm,
        min_distance; /* distance in original-track seconds */
    int group_count;
    cues_group groups[CUES_MAX_GROUPS];
} cues_config;
typedef struct {
    uint32_t classification, slot, type, rgb;
    double start_s, end_s;
    char comment[128];
} cues_point;
typedef struct {
    int count, skipped;
    cues_point points[CUES_MAX_PLAN];
} cues_plan;
void cues_config_default(cues_config *config);
void cues_group_default(cues_group *group, const cues_config *config);
void cues_rule_default(cues_rule *rule, const cues_group *group);
int cues_config_validate(const cues_config *config, char *error, size_t size);
/* JSON is bounded to 128 KiB. Parse failures leave the destination unchanged.
 */
int cues_config_parse(cues_config *config, const char *json, size_t length,
                      char *error, size_t size);
int cues_config_json(const cues_config *config, char *json, size_t capacity);
extern const uint32_t cues_colors[9];
extern const char *const cues_color_names[9];
extern const char *const cues_phrase_names[8];
/* Pure evaluator. Actual source sample rate is mandatory. Existing native cues
 * are preserved; no command seeks or edits working/retained playback state. */
int cues_build_plan(const cues_config *config, const az_track_snapshot *track,
                    const az_native_cue_snapshot *existing,
                    uint32_t sample_rate, cues_plan *out);
/* Native settings + worker-owned JSON storage. Called on UI except store
 * worker. */
const az_screen_v1 *cues_settings_init(const az_core_v1 *core,
                                       void (*changed)(void));
const cues_config *cues_settings_config(void);
void cues_settings_status(const char *status);
/* Drives bounded handoff of persistence completions, never file IO on UI. */
void cues_settings_tick(void);
#endif
