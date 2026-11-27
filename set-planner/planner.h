/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SET_PLANNER_PLANNER_H
#define SET_PLANNER_PLANNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char title[192], artist[128], key_text[32];
    double bpm, energy, duration;
    int key;
    bool major, energy_known;
} SpTrack;

enum { SP_ARC_POINTS = 4 };
typedef struct { double position, energy; } SpAnchor;
typedef struct { SpAnchor points[SP_ARC_POINTS]; int count; } SpArc;

typedef struct {
    int curve;
    double max_bpm;
    bool halftime, time_aware;
    int opener, closer;
    SpArc custom_arc;
    bool (*cancelled)(void *user);
    void *user;
} SpOptions;

typedef struct { int entry, from, to; } SpMove;
typedef struct {
    double bpm_gap;
    int key_steps;
    bool tempo_warning, key_warning;
} SpTransition;
typedef struct {
    double score, max_bpm, max_raw_bpm, energy_error;
    int compatible, key_warnings, unknown_keys, tempo_jumps, unknown_bpm;
} SpMetrics;

enum { SP_OPENING, SP_PRIMETIME, SP_CLOSING, SP_FULL_NIGHT, SP_B2B, SP_CURVES };
extern const char *const sp_curve_names[SP_CURVES];
const SpArc *sp_arc(const SpOptions *options);
double sp_arc_energy(const SpArc *arc, double position);
double sp_track_position(int index, int count, double center, double duration);
void sp_track_key(SpTrack *track, const char *key);
double sp_tempo_gap(const SpTrack *a, const SpTrack *b, bool halftime);
SpTransition sp_transition(const SpTrack *a, const SpTrack *b, const SpOptions *options);
bool sp_normalize_energy(SpTrack *tracks, int count);
bool sp_plan_order(const SpTrack *tracks, int count, const SpOptions *options, int *out);
SpMetrics sp_measure(const SpTrack *tracks, int count, const int *order, const SpOptions *options);
int sp_plan_moves(const int *target, int count, SpMove *out, const SpOptions *options);
void sp_relocate(int *order, int from, int to);

#endif
