/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod.h"
#include <math.h>
#include <stdlib.h>

static void add_track(SpTimeline *timeline, const SpTrack *track, int index)
{
    timeline->duration += track->duration;
    timeline->starts[index + 1] = timeline->duration;
    timeline->partial_duration |= track->duration <= 0;
    if (track->bpm > 0) {
        timeline->bpm_low = fmin(timeline->bpm_low, track->bpm);
        timeline->bpm_high = fmax(timeline->bpm_high, track->bpm);
    }
}

static const SpTrack *planned_track(const SpModel *model, int index)
{
    return &model->tracks[model->target[index]];
}

static void add_bin_sample(SpChartBin *bin, const SpTrack *track)
{
    if (track->energy_known) {
        bin->energy_low = bin->energy_count ? fmin(bin->energy_low, track->energy) : track->energy;
        bin->energy_high = fmax(bin->energy_high, track->energy);
        bin->energy_total += track->energy;
        ++bin->energy_count;
    }
    if (track->bpm > 0) {
        bin->bpm_total += track->bpm;
        ++bin->bpm_count;
    }
}

static void build_overview(SpModel *model, const SpOptions *options, int count)
{
    for (int i = 0; i < count; ++i) {
        double center = (sp_chart_boundary(model, i) + sp_chart_boundary(model, i + 1)) / 2;
        int index = (int)sp_clamp(center * SP_OVERVIEW_BINS, 0, SP_OVERVIEW_BINS - 1);
        SpChartBin *bin = &model->timeline.bins[index];
        const SpTrack *track = planned_track(model, i);
        add_bin_sample(bin, track);
        if (i > 0) {
            SpTransition step = sp_transition(planned_track(model, i - 1), track, options);
            ++bin->transitions;
            bin->key_warnings += step.key_warning;
            bin->tempo_warnings += step.tempo_warning;
        }
    }
}

bool sp_build_timeline(SpModel *model, const SpOptions *options)
{
    int count = (int)model->source.identity.total;
    SpTimeline *timeline = &model->timeline;
    timeline->starts = calloc((size_t)count + 1, sizeof *timeline->starts);
    if (!timeline->starts)
        return false;
    timeline->bpm_low = INFINITY;
    for (int i = 0; i < count; ++i)
        add_track(timeline, &model->tracks[model->target[i]], i);
    timeline->time_axis = options->time_aware && timeline->duration > 0;
    if (count > SP_OVERVIEW_LIMIT)
        build_overview(model, options, count);
    model->original = sp_measure(model->tracks, count, NULL, options);
    model->planned = sp_measure(model->tracks, count, model->target, options);
    return true;
}

double sp_chart_boundary(const SpModel *model, int index)
{
    const SpTimeline *timeline = &model->timeline;
    if (timeline->time_axis)
        return timeline->starts[index] / timeline->duration;
    return (double)index / model->source.identity.total;
}

int sp_chart_track(const SpModel *model, double position)
{
    int low = 0, high = (int)model->source.identity.total;
    while (high - low > 1) {
        int middle = low + (high - low) / 2;
        if (sp_chart_boundary(model, middle) <= position)
            low = middle;
        else
            high = middle;
    }
    return low;
}

double sp_chart_target(const SpRuntime *runtime, int index)
{
    const SpModel *model = &runtime->model;
    const SpTimeline *timeline = &model->timeline;
    double center = (timeline->starts[index] + timeline->starts[index + 1]) / 2;
    double duration = timeline->time_axis ? timeline->duration : 0;
    double position = sp_track_position(index, (int)model->source.identity.total, center, duration);
    return sp_arc_energy(sp_arc(&runtime->options), position);
}

static void chart_first(SpRuntime *r, int first)
{
    int last_start = (int)r->model.source.identity.total - SP_DETAIL_TRACKS;
    r->chart_first = (int)sp_clamp(first, 0, last_start);
}

void sp_chart_page(SpRuntime *r, int direction)
{
    chart_first(r, r->chart_first + direction * (SP_DETAIL_TRACKS / 2));
}

void sp_chart_scroll_to(SpRuntime *r, double position)
{
    int count = (int)r->model.source.identity.total;
    chart_first(r, (int)lround(count * sp_clamp(position, 0, 1)) - SP_DETAIL_TRACKS / 2);
}

void sp_chart_follow(SpRuntime *r)
{
    int index = r->cursor - 1;
    if (!r->chart_detail || index < 0)
        return;
    if (index < r->chart_first || index >= r->chart_first + SP_DETAIL_TRACKS)
        chart_first(r, index - SP_DETAIL_TRACKS / 2);
}

int sp_chart_pick_index(const SpRuntime *r, double position)
{
    if (!r->chart_detail)
        return sp_chart_track(&r->model, position);
    int offset = (int)sp_clamp(position * SP_DETAIL_TRACKS, 0, SP_DETAIL_TRACKS - 1);
    return r->chart_first + offset;
}
