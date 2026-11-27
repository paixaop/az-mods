/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui.h"
#include <math.h>
#include <stdio.h>

static int energy_y(az_rect plot, double energy)
{
    return plot.y + (int)lround(plot.h * (10 - energy) / 9);
}

static int chart_x(az_rect plot, double position)
{
    return plot.x + (int)lround(plot.w * position);
}

static void energy_grid(const Paint *p, az_rect plot)
{
    const int levels[] = {10, 7, 4, 1};
    for (size_t i = 0; i < sizeof levels / sizeof *levels; ++i) {
        int y = energy_y(plot, levels[i]);
        char label[8];
        snprintf(label, sizeof label, "%d", levels[i]);
        sp_label(p, plot.x - 35, y - 10, label, 14, 30, p->dim);
        sp_rect(p, plot.x, y, plot.w + 1, 1, p->rule);
    }
}

static void dashed(const Paint *p, int x, int y, int end, int next_y)
{
    for (int start = x; start < end; start += 12) {
        int stop = start + 7 < end ? start + 7 : end;
        int dy = next_y - y;
        int a = y + dy * (start - x) / (end - x);
        int b = y + dy * (stop - x) / (end - x);
        sp_line(p, start, a, stop, b, 2, p->target);
    }
}

static az_rect target_plot(const SpRuntime *r, az_rect plot)
{
    int count = (int)r->model.source.identity.total;
    if (!r->model.timeline.time_axis && count > 1) {
        int margin = plot.w / (2 * count);
        plot.x += margin;
        plot.w -= 2 * margin;
    }
    return plot;
}

static void target_curve(const Paint *p, az_rect plot, bool editable)
{
    const SpArc *arc = sp_arc(&p->runtime->options);
    for (int i = 1; i < arc->count; ++i) {
        SpAnchor a = arc->points[i - 1], b = arc->points[i];
        int x = chart_x(plot, a.position), end = chart_x(plot, b.position);
        int y = energy_y(plot, a.energy), next = energy_y(plot, b.energy);
        if (editable)
            sp_line(p, x, y, end, next, 3, p->energy);
        else
            dashed(p, x, y, end, next);
    }
}

SpSetupLayout sp_setup_layout(az_rect area)
{
    int x = area.x + area.w * 46 / 100, y = area.y + SP_HEADER + SP_INSET;
    int w = area.x + area.w - x - SP_INSET;
    return (SpSetupLayout){.card = {x, y, w, 403},
        .plot = {x + 59, y + 126, w - 93, 175},
        .preset = {x + 19, y + 346, 236, 41},
        .reset = {x + 267, y + 346, 153, 41}};
}

static void setup_button(const Paint *p, az_rect button, const char *value)
{
    sp_rect(p, button.x, button.y, button.w, button.h, p->head);
    sp_label(p, button.x + 12, button.y + 8, value, 17, button.w - 24, p->text);
}

static void dot(const Paint *p, int x, int y, int radius, uint32_t color)
{
    for (int row = -radius; row <= radius; ++row) {
        int half = (int)sqrt(radius * radius - row * row);
        sp_rect(p, x - half, y + row, 2 * half + 1, 1, color);
    }
}

static void setup_anchors(const Paint *p, az_rect plot)
{
    SpRuntime *r = p->runtime;
    const SpArc *arc = sp_arc(&r->options);
    r->arc_plot = plot;
    for (int i = 0; i < arc->count; ++i) {
        SpAnchor point = arc->points[i];
        int x = chart_x(plot, point.position), y = energy_y(plot, point.energy);
        dot(p, x, y, 9, p->energy);
        dot(p, x, y, 5, p->bg);
        sp_publish_anchor(r, i, x, y);
    }
}

void sp_draw_setup_arc(const Paint *p, const char *energy_source)
{
    const SpOptions *options = &p->runtime->options;
    SpSetupLayout layout = sp_setup_layout(p->runtime->area);
    az_rect box = layout.card, plot = layout.plot;
    sp_card(p, box);
    const char *name = options->custom_arc.count ? "Custom" : sp_curve_names[options->curve];
    sp_label(p, box.x + 19, box.y + 14, "Target energy", 24, 220, p->text);
    sp_label(p, box.x + 19, box.y + 51, "Drag the dots to shape the set.", 18, box.w - 38, p->dim);
    sp_label(p, box.x + 19, box.y + 79, energy_source, 16, box.w - 38, p->dim);
    energy_grid(p, plot);
    target_curve(p, plot, true);
    setup_anchors(p, plot);
    sp_label(p, plot.x, plot.y + plot.h + 10, "Start", 15, 100, p->dim);
    sp_label(p, plot.x + plot.w - 67, plot.y + plot.h + 10, "End of set", 15, 84, p->dim);
    char preset[64];
    snprintf(preset, sizeof preset, "Curve: %s   >", name);
    setup_button(p, layout.preset, preset);
    if (options->custom_arc.count)
        setup_button(p, layout.reset, "Reset to preset");
}

typedef struct {
    const Paint *paint;
    az_rect plot;
    int bpm_y, keys_y;
    bool aggregate;
} Chart;

static const SpTrack *track_at(const SpModel *model, int index)
{
    return &model->tracks[model->target[index]];
}

static void track_energy(const Chart *chart, const SpTrack *track, int x, int end)
{
    if (!track->energy_known)
        return;
    const Paint *p = chart->paint;
    int y = energy_y(chart->plot, track->energy);
    int bottom = chart->plot.y + chart->plot.h;
    sp_rect(p, x, y + 2, end - x, bottom - y - 1, sp_mix(p->stripe, p->energy, 20));
    sp_rect(p, x, y, end - x, 2, p->energy);
}

static int tempo_y(const Chart *chart, double bpm)
{
    const SpTimeline *t = &chart->paint->runtime->model.timeline;
    double range = t->bpm_high - t->bpm_low;
    double position = range > 0 ? (t->bpm_high - bpm) / range : 0.5;
    return chart->bpm_y + 3 + (int)lround(20 * position);
}

static void track_tempo(const Chart *chart, const SpTrack *track, const SpTrack *previous, int x, int end)
{
    if (track->bpm <= 0)
        return;
    const Paint *p = chart->paint;
    int y = tempo_y(chart, track->bpm);
    sp_rect(p, x, y, end - x, 2, p->tempo);
    if (previous && previous->bpm > 0)
        sp_line(p, x, y, x, tempo_y(chart, previous->bpm), 1, p->tempo);
}

static void key_marker(const Chart *chart, SpTransition step, int x, int end)
{
    const Paint *p = chart->paint;
    if (end - x < 6)
        return;
    uint32_t color = step.key_warning ? p->clash : p->warning;
    if (step.key_warning || step.tempo_warning)
        sp_rect(p, x, chart->keys_y - 3, 3, 22, color);
    else if (step.key_steps >= 0 && step.key_steps <= 1)
        sp_rect(p, x, chart->keys_y - 3, 2, 22, p->good);
}

static void track_key(const Chart *chart, const SpTrack *track, const SpTrack *previous, int x, int end)
{
    const Paint *p = chart->paint;
    if (end - x >= 30 && track->key) {
        char key[8];
        snprintf(key, sizeof key, "%d%c", track->key, track->major ? 'B' : 'A');
        sp_label(p, x + 6, chart->keys_y - 2, key, 13, end - x - 7, p->text);
    }
    if (previous)
        key_marker(chart, sp_transition(previous, track, &p->runtime->options), x, end);
}

static void chart_span(const Chart *chart, int index, int x, int end)
{
    const SpModel *model = &chart->paint->runtime->model;
    const SpTrack *track = track_at(model, index);
    const SpTrack *previous = index > 0 ? track_at(model, index - 1) : NULL;
    track_energy(chart, track, x, end);
    if (previous && previous->energy_known && track->energy_known)
        sp_line(chart->paint, x, energy_y(chart->plot, previous->energy),
                x, energy_y(chart->plot, track->energy), 2, chart->paint->energy);
    track_tempo(chart, track, previous, x, end);
    track_key(chart, track, previous, x, end);
}

static void chart_spans(const Chart *chart)
{
    const SpModel *model = &chart->paint->runtime->model;
    az_rect plot = chart->plot;
    for (int x = 0; x < plot.w;) {
        int index = sp_chart_track(model, (x + 0.5) / plot.w);
        int end = (int)lround(plot.w * sp_chart_boundary(model, index + 1));
        if (end <= x)
            end = x + 1;
        chart_span(chart, index, plot.x + x, plot.x + end);
        x = end;
    }
}

static void detail_spans(const Chart *chart)
{
    const SpRuntime *r = chart->paint->runtime;
    for (int i = 0; i < SP_DETAIL_TRACKS; ++i) {
        int x = chart_x(chart->plot, (double)i / SP_DETAIL_TRACKS);
        int end = chart_x(chart->plot, (double)(i + 1) / SP_DETAIL_TRACKS);
        chart_span(chart, r->chart_first + i, x, end);
    }
}

static void detail_target(const Chart *chart)
{
    const Paint *p = chart->paint;
    const SpRuntime *r = p->runtime;
    az_rect plot = chart->plot;
    int left = plot.x, y = energy_y(plot, sp_chart_target(r, r->chart_first));
    for (int i = 0; i < SP_DETAIL_TRACKS; ++i) {
        int x = chart_x(plot, (i + 0.5) / SP_DETAIL_TRACKS);
        int next = energy_y(plot, sp_chart_target(r, r->chart_first + i));
        dashed(p, left, y, x, next);
        left = x;
        y = next;
    }
    dashed(p, left, y, plot.x + plot.w, y);
}

static void bin_energy(const Chart *chart, const SpChartBin *bin, const SpChartBin *previous, int x, int end)
{
    if (!bin->energy_count)
        return;
    const Paint *p = chart->paint;
    az_rect plot = chart->plot;
    int high = energy_y(plot, bin->energy_high), low = energy_y(plot, bin->energy_low);
    int y = energy_y(plot, bin->energy_total / bin->energy_count);
    int center = (x + end) / 2;
    sp_rect(p, x, high, end - x, low - high + 1, sp_mix(p->stripe, p->energy, 13));
    if (!previous || !previous->energy_count)
        sp_rect(p, x, y, center - x + 1, 2, p->energy);
    else {
        int last_y = energy_y(plot, previous->energy_total / previous->energy_count);
        sp_line(p, center - (end - x), last_y, center, y, 2, p->energy);
    }
}

static void bin_tempo(const Chart *chart, const SpChartBin *bin, const SpChartBin *previous, int x, int end)
{
    if (!bin->bpm_count)
        return;
    const Paint *p = chart->paint;
    int y = tempo_y(chart, bin->bpm_total / bin->bpm_count), center = (x + end) / 2;
    if (!previous || !previous->bpm_count)
        sp_rect(p, x, y, center - x + 1, 2, p->tempo);
    else {
        int last_y = tempo_y(chart, previous->bpm_total / previous->bpm_count);
        sp_line(p, center - (end - x), last_y, center, y, 2, p->tempo);
    }
}

static void bin_flags(const Chart *chart, const SpChartBin *bin, int x, int end)
{
    if (!bin->transitions)
        return;
    const Paint *p = chart->paint;
    double scale = (end - x - 1) / (double)bin->transitions;
    sp_rect(p, x, chart->keys_y, (int)ceil(scale * bin->tempo_warnings), 6, p->warning);
    sp_rect(p, x, chart->keys_y + 8, (int)ceil(scale * bin->key_warnings), 6, p->clash);
}

static void overview_spans(const Chart *chart)
{
    const SpChartBin *bins = chart->paint->runtime->model.timeline.bins;
    for (int i = 0; i < SP_OVERVIEW_BINS; ++i) {
        int x = chart_x(chart->plot, (double)i / SP_OVERVIEW_BINS);
        int end = chart_x(chart->plot, (double)(i + 1) / SP_OVERVIEW_BINS);
        const SpChartBin *previous = i > 0 ? &bins[i - 1] : NULL;
        bin_energy(chart, &bins[i], previous, x, end);
        bin_tempo(chart, &bins[i], previous, x, end);
        bin_flags(chart, &bins[i], x, end);
    }
}

static void format_time(char *label, size_t size, double duration, double fraction)
{
    int seconds = (int)lround(fmin(duration * fraction, 99999999));
    if (duration >= 3600)
        snprintf(label, size, "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else
        snprintf(label, size, "%d:%02d", seconds / 60, seconds % 60);
}

static void axis_mark(const Chart *chart, int x, double fraction, int available)
{
    const Paint *p = chart->paint;
    const SpModel *model = &p->runtime->model;
    char label[32];
    if (model->timeline.time_axis && !p->runtime->chart_detail)
        format_time(label, sizeof label, model->timeline.duration, fraction);
    else
        snprintf(label, sizeof label, "#%d", sp_chart_pick_index(p->runtime, fraction) + 1);
    sp_label(p, x, chart->keys_y + 22, label, 14, available, p->dim);
}

static void chart_axis(const Chart *chart)
{
    az_rect plot = chart->plot;
    axis_mark(chart, plot.x, 0, 66);
    const Paint *p = chart->paint;
    if (p->runtime->model.source.identity.total > 1) {
        axis_mark(chart, plot.x + plot.w / 2 - 18, 0.5, 68);
        axis_mark(chart, plot.x + plot.w - 57, 1, 74);
    }
}

static void draw_cursor(const Chart *chart)
{
    const Paint *p = chart->paint;
    int index = p->runtime->cursor - 1;
    if (index < 0)
        return;
    const SpRuntime *r = p->runtime;
    const SpModel *model = &r->model;
    double center = r->chart_detail ? (index - r->chart_first + 0.5) / SP_DETAIL_TRACKS :
        (sp_chart_boundary(model, index) + sp_chart_boundary(model, index + 1)) / 2;
    if (center < 0 || center > 1)
        return;
    int x = chart_x(chart->plot, center);
    uint32_t color = sp_mix(p->stripe, p->text, 52);
    sp_rect(p, x, chart->plot.y - 4, 1, chart->keys_y + 19 - chart->plot.y, color);
    sp_rect(p, x - 2, chart->plot.y - 5, 5, 4, p->text);
}

static void chart_heading(const Chart *chart, az_rect box)
{
    const Paint *p = chart->paint;
    const SpOptions *options = &p->runtime->options;
    const char *name = options->custom_arc.count ? "Custom curve" : sp_curve_names[options->curve];
    const char *heading = p->runtime->chart_detail ? "Set detail" : "Set overview";
    sp_label(p, box.x + 17, box.y + 10, heading, 24, 181, p->text);
    sp_label(p, box.x + 191, box.y + 15, name, 16, box.w - 210, p->dim);
    int y = box.y + 49, x = box.x + 60;
    if (chart->aggregate)
        sp_rect(p, x, y + 4, 22, 10, sp_mix(p->stripe, p->energy, 23));
    sp_rect(p, x, y + 8, 22, 3, p->energy);
    sp_label(p, x + 29, y, chart->aggregate ? "Avg. energy / range" : "Planned energy", 15, 163, p->text);
    sp_rect(p, x + 199, y + 8, 8, 2, p->target);
    sp_rect(p, x + 212, y + 8, 8, 2, p->target);
    sp_label(p, x + 227, y, "Target energy", 15, 118, p->dim);
}

static void chart_data_label(const Paint *p, az_rect box)
{
    const SpTimeline *timeline = &p->runtime->model.timeline;
    int x = box.x + box.w - 174;
    char range[42] = "BPM unavailable";
    if (isfinite(timeline->bpm_low))
        snprintf(range, sizeof range, "%.1f - %.1f BPM", timeline->bpm_low, timeline->bpm_high);
    sp_label(p, x, box.y + 49, range, 15, 160, p->tempo);
    if (timeline->time_axis && timeline->partial_duration && !p->runtime->chart_detail)
        sp_label(p, x, box.y + 68, "Known durations", 13, 160, p->dim);
}

static void chart_button(const Paint *p, az_rect box, const char *value, bool enabled)
{
    int width = p->core->text_width(value, 16);
    sp_rect(p, box.x, box.y, box.w, box.h, enabled ? p->select : p->head);
    sp_label(p, box.x + (box.w - width) / 2, box.y + 5, value, 16, box.w - 7,
             enabled ? p->text : p->dim);
}

static void chart_rail(const Paint *p, az_rect rail)
{
    SpRuntime *r = p->runtime;
    int count = (int)r->model.source.identity.total;
    int x = chart_x(rail, (double)r->chart_first / count);
    int end = chart_x(rail, (double)(r->chart_first + SP_DETAIL_TRACKS) / count);
    int thumb = (int)sp_clamp(end - x, 7, rail.w);
    x = (int)sp_clamp(x, rail.x, rail.x + rail.w - thumb);
    sp_rect(p, rail.x, rail.y + 7, rail.w, 7, p->rule);
    sp_rect(p, x, rail.y + 4, thumb, 13, p->energy);
    sp_label(p, rail.x - 49, rail.y + 1, "Scroll", 13, 44, p->dim);
    if (!r->confirmation_pending)
        sp_publish_rail(r, rail);
}

static void chart_controls(const Paint *p, SpReviewLayout layout)
{
    const SpRuntime *r = p->runtime;
    int count = (int)r->model.source.identity.total;
    if (count <= SP_DETAIL_TRACKS)
        return;
    chart_button(p, layout.zoom, r->chart_detail ? "Full set" : "Zoom in", true);
    if (r->chart_detail) {
        chart_button(p, layout.left, "<", r->chart_first > 0);
        chart_button(p, layout.right, ">", r->chart_first + SP_DETAIL_TRACKS < count);
        chart_rail(p, layout.rail);
    }
}

static void chart_series(const Chart *chart)
{
    const Paint *p = chart->paint;
    if (p->runtime->chart_detail) {
        detail_spans(chart);
        detail_target(chart);
    } else {
        if (chart->aggregate)
            overview_spans(chart);
        else
            chart_spans(chart);
        target_curve(p, target_plot(p->runtime, chart->plot), false);
    }
    draw_cursor(chart);
}

static void chart_lanes(const Chart *chart, int x)
{
    const Paint *p = chart->paint;
    if (chart->aggregate)
        sp_label(p, x, chart->bpm_y - 5, "Avg.", 11, 38, p->tempo);
    int offset = chart->aggregate ? 7 : 3;
    sp_label(p, x, chart->bpm_y + offset, "BPM", 15, 45, p->tempo);
    sp_label(p, x, chart->keys_y - 3, chart->aggregate ? "Flags" : "KEY", 15, 45, p->dim);
}

void sp_draw_set_chart(const Paint *p, SpReviewLayout layout)
{
    az_rect plot = layout.plot;
    int bottom = plot.y + plot.h;
    bool aggregate = !p->runtime->chart_detail &&
        p->runtime->model.source.identity.total > SP_OVERVIEW_LIMIT;
    Chart chart = {.paint = p, .plot = plot, .bpm_y = bottom + 15, .keys_y = bottom + 56,
        .aggregate = aggregate};
    sp_card(p, layout.chart);
    chart_heading(&chart, layout.chart);
    chart_data_label(p, layout.chart);
    energy_grid(p, plot);
    sp_rect(p, plot.x, chart.bpm_y + 30, plot.w, 1, p->rule);
    sp_rect(p, plot.x, chart.keys_y - 4, plot.w, 23, p->head);
    chart_series(&chart);
    chart_lanes(&chart, layout.chart.x + 13);
    chart_axis(&chart);
    chart_controls(p, layout);
}
