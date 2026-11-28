/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui.h"
#include <math.h>
#include <stdio.h>

SpReviewLayout sp_review_layout(const SpRuntime *r)
{
    az_rect area = r->area;
    int x = area.x + SP_INSET, y = area.y + SP_HEADER + 12;
    int w = area.w * 61 / 100 - SP_INSET, h = area.h - SP_FOOTER - 11 - (y - area.y);
    int list_x = x + w + 12, chart_h = h * 63 / 100;
    int zoom_x = x + w - 111;
    int extra = r->chart_detail ? SP_RAIL_HEIGHT + 3 : 0;
    return (SpReviewLayout){.chart = {x, y, w, chart_h},
        .detail = {x, y + chart_h + 10, w, h - chart_h - 10},
        .list = {list_x, y, area.x + area.w - list_x - SP_INSET, h},
        .plot = {x + 59, y + 88, w - 83, chart_h - 187 - extra},
        .zoom = {zoom_x, y + 8, 99, 34},
        .left = {zoom_x - 91, y + 8, 39, 34},
        .right = {zoom_x - 46, y + 8, 39, 34},
        .rail = {x + 59, y + chart_h - SP_RAIL_HEIGHT - 2, w - 83, SP_RAIL_HEIGHT}};
}

static const SpTrack *planned_track(const SpModel *model, int index)
{
    return &model->tracks[model->target[index]];
}

static az_rect track_row(az_rect list, int offset)
{
    return (az_rect){list.x + 1, list.y + SP_LIST_HEADER + offset * SP_ROW, list.w - 2, SP_ROW};
}

static void energy_meter(const Paint *p, az_rect row, const SpInput *input)
{
    int x = row.x + row.w - 56, y = row.y;
    char value[24];
    sp_energy_value(p->runtime, input, value, sizeof value);
    sp_label(p, x, y + 5, value, 16, 49, p->text);
    sp_rect(p, x, y + 35, 38, 4, p->rule);
    if (input->energy != SP_MISSING)
        sp_rect(p, x, y + 35, (int)lround(38 * input->score), 4, p->energy);
}

static void list_warning(const Paint *p, az_rect row, int index)
{
    if (index <= 0)
        return;
    const SpRuntime *r = p->runtime;
    SpTransition step = sp_transition(planned_track(&r->model, index - 1),
                                       planned_track(&r->model, index), &r->options);
    if (step.key_warning || step.tempo_warning)
        sp_rect(p, row.x + 6, row.y + 20, 5, 12, step.key_warning ? p->clash : p->warning);
}

static void review_row(const Paint *p, az_rect row, int index)
{
    const SpRuntime *r = p->runtime;
    int entry = r->model.target[index], end = row.x + row.w;
    const SpTrack *track = &r->model.tracks[entry];
    bool selected = r->cursor == index + 1;
    uint32_t bg = selected ? p->select : (index % 2 ? p->head : p->stripe);
    sp_rect(p, row.x, row.y, row.w, row.h - 1, bg);
    if (selected)
        sp_rect(p, row.x, row.y, 3, row.h - 1, p->energy);
    char number[16], bpm[20] = "-";
    snprintf(number, sizeof number, "%d", index + 1);
    if (track->bpm > 0)
        snprintf(bpm, sizeof bpm, "%.1f", track->bpm);
    list_warning(p, row, index);
    sp_draw_sequence_mark(p, entry, row.x + 10, row.y, row.h);
    sp_label(p, row.x + 20, row.y + 14, number, 16, 39, p->dim);
    sp_label(p, row.x + 62, row.y + 4, sp_display_title(track), 18, row.w - 247, p->text);
    sp_label(p, row.x + 62, row.y + 29, track->artist, 14, row.w - 247, p->dim);
    sp_label(p, end - 174, row.y + 14, bpm, 17, 65, p->tempo);
    sp_label(p, end - 105, row.y + 14, *track->key_text ? track->key_text : "-", 16, 45, p->text);
    energy_meter(p, row, &r->model.inputs[entry]);
}

static void review_scrollbar(const Paint *p, az_rect list, int total, int start)
{
    int visible = p->runtime->visible;
    if (total <= visible)
        return;
    int height = visible * SP_ROW, thumb = (int)sp_clamp(height * visible / total, 12, height);
    int y = list.y + SP_LIST_HEADER + (height - thumb) * start / (total - visible);
    sp_rect(p, list.x + list.w - 4, y, 3, thumb, p->dim);
}

static void list_heading(const Paint *p, az_rect box)
{
    int end = box.x + box.w;
    sp_label(p, box.x + 16, box.y + 8, "Running order", 23, 210, p->text);
    unsigned count = p->runtime->model.source.identity.total;
    char value[32];
    snprintf(value, sizeof value, "%u tracks", count);
    sp_label(p, end - 110, box.y + 13, value, 16, 96, p->dim);
    sp_label(p, box.x + 63, box.y + 43, "Track", 14, 90, p->dim);
    sp_label(p, end - 175, box.y + 43, "BPM", 14, 60, p->dim);
    sp_label(p, end - 106, box.y + 43, "Key", 14, 45, p->dim);
    sp_label(p, end - 64, box.y + 43, "Energy", 14, 59, p->dim);
    sp_rect(p, box.x + 1, box.y + SP_LIST_HEADER - 2, box.w - 2, 1, p->rule);
}

static void review_list(const Paint *p, az_rect list)
{
    const SpRuntime *r = p->runtime;
    int count = (int)r->model.source.identity.total;
    int start = r->top > 1 ? r->top - 1 : 0;
    sp_card(p, list);
    list_heading(p, list);
    for (int i = 0; i < r->visible && start + i < count; ++i)
        review_row(p, track_row(list, i), start + i);
    review_scrollbar(p, list, count, start);
}

static az_rect detail_tab(az_rect box, bool compare)
{
    return (az_rect){box.x + (compare ? 205 : 12), box.y + 5, compare ? 148 : 189, 36};
}

static void draw_tab(const Paint *p, az_rect box, bool compare)
{
    az_rect tab = detail_tab(box, compare);
    bool current = p->runtime->compare == compare;
    sp_label(p, tab.x + 7, tab.y + 5, compare ? "Before / after" : "Transition", 19,
             tab.w - 14, current ? p->text : p->dim);
    if (current)
        sp_rect(p, tab.x + 7, tab.y + 32, tab.w - 14, 2, p->energy);
}

static const char *key_relation(const SpTrack *a, const SpTrack *b, SpTransition step)
{
    if (step.key_steps < 0)
        return "Key unavailable";
    if (step.key_warning)
        return "Key clash";
    if (step.key_steps == 0)
        return a->major == b->major ? "Same key" : "Relative key";
    return step.key_steps == 1 ? "Adjacent keys" : "Distant keys";
}

static void energy_detail(const Paint *p, const SpTrack *a, const SpTrack *b, int index, char *out, size_t size)
{
    double target = sp_chart_target(p->runtime, index);
    char first[16] = "-", second[16] = "-";
    if (a && a->energy_known)
        snprintf(first, sizeof first, "%.1f", a->energy);
    if (b->energy_known)
        snprintf(second, sizeof second, "%.1f", b->energy);
    if (a)
        snprintf(out, size, "Energy %s to %s  /  Target %.1f", first, second, target);
    else
        snprintf(out, size, "Energy %s  /  Target %.1f", second, target);
}

static void tempo_detail(const SpTrack *a, const SpTrack *b, SpTransition step, char *out, size_t size)
{
    if (!isfinite(step.bpm_gap)) {
        snprintf(out, size, "BPM unavailable");
        return;
    }
    snprintf(out, size, "%.1f to %.1f BPM (%+.1f%%)  /  %s%.1f BPM gap", a->bpm,
             b->bpm, 100 * (b->bpm - a->bpm) / a->bpm, step.tempo_warning ? "Jump " : "", step.bpm_gap);
}

static void opening_track(const Paint *p, az_rect box, const SpTrack *track)
{
    char energy[96];
    sp_label(p, box.x + 19, box.y + 48, sp_display_title(track), 22, box.w - 38, p->text);
    energy_detail(p, NULL, track, 0, energy, sizeof energy);
    sp_label(p, box.x + 19, box.y + 91, energy, 17, box.w - 38, p->energy);
}

static void transition_detail(const Paint *p, az_rect box, int index)
{
    const SpRuntime *r = p->runtime;
    const SpTrack *b = planned_track(&r->model, index);
    if (!index) {
        opening_track(p, box, b);
        return;
    }
    const SpTrack *a = planned_track(&r->model, index - 1);
    SpTransition step = sp_transition(a, b, &r->options);
    int x = box.x + 19, width = box.w - 38;
    char label[400], tempo[96], energy[96];
    snprintf(label, sizeof label, "%s  >  %s", sp_display_title(a), sp_display_title(b));
    sp_label(p, x, box.y + 46, label, 20, width, p->text);
    snprintf(label, sizeof label, "%s  >  %s   /   %s", *a->key_text ? a->key_text : "-",
             *b->key_text ? b->key_text : "-", key_relation(a, b, step));
    sp_label(p, x, box.y + 75, label, 16, width, step.key_warning ? p->clash : p->text);
    tempo_detail(a, b, step, tempo, sizeof tempo);
    sp_label(p, x, box.y + 99, tempo, 16, width, step.tempo_warning ? p->warning : p->tempo);
    energy_detail(p, a, b, index, energy, sizeof energy);
    sp_label(p, x, box.y + 123, energy, 16, width, p->energy);
}

static void compare_row(const Paint *p, az_rect box, int y, const char *metric, const char *before, const char *after)
{
    int x = box.x + 19;
    sp_label(p, x, y, metric, 16, box.w / 2 - 20, p->text);
    sp_label(p, box.x + box.w * 57 / 100, y, before, 16, 110, p->dim);
    sp_label(p, box.x + box.w * 78 / 100, y, after, 16, 110, p->energy);
}

static void compare_numbers(const Paint *p, az_rect box, int y, const char *metric, int before, int after)
{
    char a[20], b[20];
    snprintf(a, sizeof a, "%d", before);
    snprintf(b, sizeof b, "%d", after);
    compare_row(p, box, y, metric, a, b);
}

static void compare_energy(const Paint *p, az_rect box, const SpModel *model)
{
    int count = (int)model->source.identity.total, known = count - model->unknown_energy;
    char a[24] = "-", b[24] = "-";
    if (known > 0 && count > 1) {
        snprintf(a, sizeof a, "%.1f", sqrt(model->original.energy_error / known));
        snprintf(b, sizeof b, "%.1f", sqrt(model->planned.energy_error / known));
    }
    compare_row(p, box, box.y + 131, "Energy deviation (RMS)", a, b);
}

static void compare_order(const Paint *p, az_rect box)
{
    const SpModel *model = &p->runtime->model;
    SpMetrics a = model->original, b = model->planned;
    sp_label(p, box.x + box.w * 57 / 100, box.y + 39, "USB order", 14, 112, p->dim);
    sp_label(p, box.x + box.w * 78 / 100, box.y + 39, "Planned", 14, 112, p->dim);
    compare_numbers(p, box, box.y + 59, "Compatible key changes", a.compatible, b.compatible);
    compare_numbers(p, box, box.y + 83, "Key clashes", a.key_warnings, b.key_warnings);
    compare_numbers(p, box, box.y + 107, "Tempo jumps", a.tempo_jumps, b.tempo_jumps);
    compare_energy(p, box, model);
}

static void review_detail(const Paint *p, az_rect box)
{
    const SpRuntime *r = p->runtime;
    sp_card(p, box);
    draw_tab(p, box, false);
    draw_tab(p, box, true);
    if (r->compare) {
        compare_order(p, box);
    } else {
        int index = r->cursor ? r->cursor - 1 : (r->model.source.identity.total > 1);
        transition_detail(p, box, index);
    }
}

void sp_draw_review(const Paint *p, SpReviewLayout layout)
{
    sp_draw_set_chart(p, layout);
    review_list(p, layout.list);
    review_detail(p, layout.detail);
}

static bool chart_pick(SpRuntime *r, SpReviewLayout layout, int x, int y)
{
    az_rect hit = layout.plot;
    hit.w += 1;
    hit.h += 78;
    if (!sp_contains(hit, x, y))
        return false;
    r->cursor = sp_chart_pick_index(r, (x - hit.x) / (double)layout.plot.w) + 1;
    r->compare = false;
    return true;
}

static void list_pick(SpRuntime *r, az_rect list, int x, int y)
{
    az_rect rows = track_row(list, 0);
    rows.h = r->visible * SP_ROW;
    if (!sp_contains(rows, x, y))
        return;
    int index = (r->top > 1 ? r->top : 1) + (y - rows.y) / SP_ROW;
    if (index <= (int)r->model.source.identity.total) {
        r->cursor = index;
        r->compare = false;
        sp_chart_follow(r);
    }
}

static bool zoom_hit(SpRuntime *r, SpReviewLayout layout, int x, int y)
{
    if (r->model.source.identity.total <= SP_DETAIL_TRACKS)
        return false;
    if (!sp_contains(layout.zoom, x, y))
        return false;
    sp_reset_drag(r);
    r->chart_detail = !r->chart_detail;
    sp_chart_follow(r);
    return true;
}

static bool pan_hit(SpRuntime *r, SpReviewLayout layout, int x, int y)
{
    if (!r->chart_detail)
        return false;
    if (sp_contains(layout.rail, x, y))
        sp_chart_scroll_to(r, (x - layout.rail.x) / (double)(layout.rail.w - 1));
    else if (sp_contains(layout.left, x, y))
        sp_chart_page(r, -1);
    else if (sp_contains(layout.right, x, y))
        sp_chart_page(r, 1);
    else
        return false;
    return true;
}

void sp_tap_review(SpRuntime *r, const az_event *event)
{
    SpReviewLayout layout = sp_review_layout(r);
    int x = event->x, y = event->y;
    if (zoom_hit(r, layout, x, y) || pan_hit(r, layout, x, y))
        return;
    if (sp_contains(detail_tab(layout.detail, true), x, y))
        r->compare = true;
    else if (sp_contains(detail_tab(layout.detail, false), x, y))
        r->compare = false;
    else if (!chart_pick(r, layout, x, y))
        list_pick(r, layout.list, x, y);
}
