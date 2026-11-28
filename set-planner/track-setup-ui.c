/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui.h"
#include <stdio.h>

enum { SEQUENCE, UNLOCK, OPENER, CLOSER, DONE, TRACK_ACTIONS };

static int selected_entry(const SpRuntime *r)
{
    return r->model.setup.order[r->cursor];
}

static bool in_sequence(const SpTrackSetup *setup, int entry)
{
    SpLink link = setup->links[entry];
    return link.previous >= 0 || link.next >= 0;
}

static az_rect action_box(az_rect area, int slot)
{
    int y = area.y + area.h - 49;
    int w = (area.w - 2 * SP_INSET) / TRACK_ACTIONS;
    return (az_rect){area.x + SP_INSET + slot * w, y, w - 5, 40};
}

static void show_action(const Paint *p, int slot, const char *label, bool primary)
{
    az_rect box = action_box(p->runtime->area, slot);
    sp_rect(p, box.x, box.y, box.w, box.h, primary ? p->action : p->bg);
    sp_label(p, box.x + 11, box.y + 8, label, 18, box.w - 22, primary ? 0xffffff : p->text);
}

static void sequence_summary(const SpRuntime *r, char *text, size_t size)
{
    const SpTrackSetup *setup = &r->model.setup;
    int written = snprintf(text, size, "%d selected", setup->draft_count);
    for (int i = 0; i < setup->draft_count && (size_t)written < size; ++i) {
        const SpTrack *track = &r->model.tracks[setup->draft[i]];
        written += snprintf(text + written, size - (size_t)written, "%s%s",
                            i ? " > " : "  |  ", sp_display_title(track));
    }
}

static void show_draft(const Paint *p, int y)
{
    const SpRuntime *r = p->runtime;
    char summary[256];
    sequence_summary(r, summary, sizeof summary);
    const char *message = r->track_hint ? r->track_hint : summary;
    sp_label(p, r->area.x + SP_INSET, y + 7, message, 19, r->area.w - 2 * SP_INSET, p->text);
    show_action(p, SEQUENCE, "Lock sequence", r->model.setup.draft_count >= 2);
    show_action(p, DONE, "Cancel", false);
}

static void show_track_actions(const Paint *p, int y)
{
    const SpRuntime *r = p->runtime;
    const SpTrackSetup *setup = &r->model.setup;
    int entry = selected_entry(r);
    bool grouped = in_sequence(setup, entry);
    bool opening = sp_sequence_head(setup->links, entry) == r->options.opener;
    bool closing = sp_sequence_tail(setup->links, entry) == r->options.closer;
    char summary[96];
    snprintf(summary, sizeof summary, "%d locked %s", setup->sequence_count,
             setup->sequence_count == 1 ? "sequence" : "sequences");
    sp_label(p, r->area.x + SP_INSET, y + 7, r->track_hint ? r->track_hint : summary,
             19, r->area.w - 2 * SP_INSET, p->text);
    show_action(p, SEQUENCE, grouped ? "Edit sequence" : "Start sequence", false);
    if (grouped)
        show_action(p, UNLOCK, "Unlock sequence", false);
    show_action(p, OPENER, opening ? "Clear opener" : "Set as opener", false);
    show_action(p, CLOSER, closing ? "Clear closer" : "Set as closer", false);
    show_action(p, DONE, "Done", true);
}

void sp_draw_track_footer(const Paint *p)
{
    const SpRuntime *r = p->runtime;
    int y = r->area.y + r->area.h - SP_TRACK_FOOTER;
    sp_rect(p, r->area.x, y, r->area.w, SP_TRACK_FOOTER, p->head);
    if (r->model.setup.drafting)
        show_draft(p, y);
    else
        show_track_actions(p, y);
}

void sp_draw_sequence_mark(const Paint *p, int entry, int x, int y, int height)
{
    const SpTrackSetup *setup = &p->runtime->model.setup;
    if (!setup->sequence_count || !in_sequence(setup, entry))
        return;
    SpLink link = setup->links[entry];
    int start = link.previous < 0 ? 6 : 0;
    int end = link.next < 0 ? height - 6 : height;
    sp_rect(p, x, y + start, 2, end - start, p->energy);
    if (start)
        sp_rect(p, x, y + start, 8, 2, p->energy);
    if (link.next < 0)
        sp_rect(p, x, y + end - 2, 8, 2, p->energy);
}

void sp_draw_track_state(const Paint *p, int entry, int x, int y, int width)
{
    const SpRuntime *r = p->runtime;
    int rank = r->model.setup.picked[entry];
    if (rank) {
        char text[24];
        snprintf(text, sizeof text, "Pick %d", rank);
        sp_label(p, x, y, text, 17, width, p->selected_text);
    } else if (entry == r->options.opener) {
        sp_label(p, x, y, "Opener", 17, width, p->energy);
    } else if (entry == r->options.closer) {
        sp_label(p, x, y, "Closer", 17, width, p->energy);
    }
}

static void find_entry(SpRuntime *r, int entry)
{
    const SpTrackSetup *setup = &r->model.setup;
    for (int i = 0; i < setup->count; ++i)
        if (setup->order[i] == entry) {
            r->cursor = i;
            return;
        }
}

void sp_select_setup_track(SpRuntime *r, bool touch)
{
    SpTrackSetup *setup = &r->model.setup;
    int entry = selected_entry(r);
    r->track_hint = NULL;
    if (!setup->drafting) {
        if (!touch)
            sp_start_sequence(setup, entry);
        return;
    }
    if (!sp_toggle_sequence_track(setup, entry))
        r->track_hint = "This track is already in another locked sequence.";
}

static void draft_action(SpRuntime *r, int slot)
{
    SpTrackSetup *setup = &r->model.setup;
    if (slot == DONE) {
        sp_cancel_sequence(setup);
        r->track_hint = NULL;
    } else if (slot == SEQUENCE) {
        int first = setup->draft_count ? setup->draft[0] : -1;
        r->track_hint = sp_lock_sequence(setup, &r->options);
        if (!r->track_hint) {
            find_entry(r, first);
            int last = setup->count - r->visible;
            r->top = last > 0 ? (int)sp_clamp(r->cursor, 0, last) : 0;
        }
    }
}

static void set_track_endpoint(SpRuntime *r, int entry, bool opening)
{
    if (sp_set_endpoint(&r->model.setup, &r->options, entry, opening))
        find_entry(r, entry);
    else
        r->track_hint = "Clear the other endpoint before pinning this sequence here.";
}

static void track_action(SpRuntime *r, int slot)
{
    SpTrackSetup *setup = &r->model.setup;
    int entry = selected_entry(r);
    r->track_hint = NULL;
    switch (slot) {
    case SEQUENCE: sp_start_sequence(setup, entry); break;
    case UNLOCK:
        if (in_sequence(setup, entry))
            sp_unlock_sequence(setup, entry);
        break;
    case OPENER: set_track_endpoint(r, entry, true); break;
    case CLOSER: set_track_endpoint(r, entry, false); break;
    case DONE: sp_open_page(r, SP_SETUP); break;
    }
}

void sp_tap_track_footer(SpRuntime *r, const az_event *event)
{
    for (int slot = 0; slot < TRACK_ACTIONS; ++slot) {
        if (!sp_contains(action_box(r->area, slot), event->x, event->y))
            continue;
        if (r->model.setup.drafting)
            draft_action(r, slot);
        else
            track_action(r, slot);
        return;
    }
}

bool sp_tap_track_scroll(SpRuntime *r, int x, int y, int start)
{
    int count = r->model.setup.count, height = r->visible * r->row_height;
    az_rect bar = {r->area.x + r->area.w - 28, start, 28, height};
    if (count <= r->visible || !sp_contains(bar, x, y))
        return false;
    r->top = (y - start) * (count - r->visible) / (height - 1);
    r->cursor = r->top;
    return true;
}
