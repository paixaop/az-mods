/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SET_PLANNER_MOD_H
#define SET_PLANNER_MOD_H

#include "azmod-mod.h"
#include "azmod-phrases.h"
#include "planner.h"
#include <stdatomic.h>

#define SP_ID "set-planner"
#define SP_MAX_ENTRIES 20000
#define SP_CACHE_PATH_MAX 512

enum SpJob { SP_NO_JOB, SP_LOAD, SP_PREVIEW, SP_REFRESH, SP_APPLY };
enum SpPhase { SP_IDLE, SP_WORKING, SP_DONE };
enum SpPage { SP_SETTINGS, SP_SETUP, SP_PICK_START, SP_PICK_END, SP_ORDER, SP_ENERGY, SP_NOTICE };
enum SpEnergy { SP_MISSING, SP_FULL, SP_PHRASE, SP_PHRASES, SP_ENERGY_COUNT };
enum SpOccurrence { SP_FIRST, SP_MIDDLE, SP_LAST, SP_OCCURRENCE_COUNT };

typedef struct {
    enum SpEnergy mode;
    enum az_phrase_kind kind;
    enum SpOccurrence occurrence;
} SpEnergyChoice;

typedef struct {
    az_playlist_order_v1 identity;
    az_playlist_entry_v1 *rows;
} SpOrder;

typedef struct {
    char path[SP_CACHE_PATH_MAX];
    double score;
    enum SpEnergy energy;
    uint32_t encoding;
} SpInput;

enum { SP_DETAIL_TRACKS = 12, SP_OVERVIEW_LIMIT = 60, SP_OVERVIEW_BINS = 48 };

typedef struct {
    double energy_total, energy_low, energy_high, bpm_total;
    unsigned energy_count, bpm_count;
    unsigned transitions, key_warnings, tempo_warnings;
} SpChartBin;

typedef struct {
    double *starts;
    double duration, bpm_low, bpm_high;
    bool time_axis, partial_duration;
    SpChartBin bins[SP_OVERVIEW_BINS];
} SpTimeline;

typedef struct {
    int stick, playlist;
    char playlist_name[192];
    SpOrder source;
    SpTrack *tracks;
    SpInput *inputs;
    int *target;
    SpMove *moves;
    int move_count, verified;
    az_playlist_move_result_v1 pending_move;
    int unknown_energy, fallback_energy, cache_hits;
    bool can_resume;
    SpMetrics original, planned;
    SpTimeline timeline;
} SpModel;

typedef struct {
    const az_core_v1 *core;
    const az_library_v1 *library;
    SpModel model;
    SpOptions options;
    SpEnergyChoice energy_choice;
    az_plan_set_target_v1 launch;
    az_surface *surface;
    bool planner_open;
    enum SpJob job;
    _Atomic enum SpPhase phase;
    _Atomic bool stop, cancel;
    _Atomic int progress, total;
    int result;
    bool sync_failed;
    bool confirmation_pending;
    enum SpPage page;
    int cursor, top, progress_seen;
    az_rect area;
    int visible, row_height;
    bool compare;
    bool chart_detail;
    int chart_first;
    az_rect arc_plot;
    az_rect chart_rail;
    uint32_t drag_generation;
    uint64_t surface_contact;
    int surface_x, surface_y;
    _Atomic uint64_t drag_handles[SP_ARC_POINTS], drag_rail, drag_event;
    uint64_t drag_contact; /* touch filter thread */
    int drag_x, drag_y;    /* touch filter thread */
    bool drag_started;
} SpRuntime;

double sp_clamp(double value, double low, double high);
bool sp_cancelled(void *user);
bool sp_stopped(const SpRuntime *runtime);
void sp_delay(void);
void sp_clear_order(SpOrder *order);
void sp_discard_plan(SpModel *model);
void sp_discard_selection(SpModel *model);
void sp_release_model(SpModel *model);
int sp_load_target(SpRuntime *runtime);
int sp_read_order(SpRuntime *runtime, SpOrder *out);
int sp_read_selection(SpRuntime *runtime);
int sp_order_check(SpRuntime *runtime);
int sp_refresh_tracks(SpRuntime *runtime);
bool sp_same_source(const SpOrder *a, const SpOrder *b);
void sp_analyze_track(SpRuntime *runtime, int index);
bool sp_cache_read(SpRuntime *runtime, int index);
void sp_cache_write(SpRuntime *runtime, int index, bool refresh);
int sp_create_preview(SpRuntime *runtime);
int sp_apply_preview(SpRuntime *runtime);
bool sp_post_job(SpRuntime *runtime, enum SpJob job);
void *sp_worker(void *user);
void sp_ui_tick(SpRuntime *runtime);
void sp_redraw(SpRuntime *runtime);
void sp_close_planner(SpRuntime *runtime);
bool sp_build_timeline(SpModel *model, const SpOptions *options);
double sp_chart_boundary(const SpModel *model, int index);
int sp_chart_track(const SpModel *model, double position);
double sp_chart_target(const SpRuntime *runtime, int index);
void sp_chart_page(SpRuntime *r, int direction);
void sp_chart_scroll_to(SpRuntime *r, double position);
void sp_chart_follow(SpRuntime *r);
int sp_chart_pick_index(const SpRuntime *r, double position);
void sp_reset_drag(SpRuntime *runtime);
int sp_touch_drag(const az_touch *touch, void *user);
void sp_publish_anchor(SpRuntime *runtime, int index, int x, int y);
void sp_publish_rail(SpRuntime *runtime, az_rect rail);
bool sp_poll_drag(SpRuntime *runtime);
bool sp_surface_drag(SpRuntime *runtime, const az_event *event);
extern const az_surface_v1 sp_surface;
extern const az_screen_v1 sp_screen;

#endif
