/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod.h"
#include <stdlib.h>
#include <string.h>

bool sp_init_track_setup(SpTrackSetup *setup, int count)
{
    setup->order = calloc((size_t)count * 4, sizeof *setup->order);
    setup->links = malloc((size_t)count * sizeof *setup->links);
    if (!setup->order || !setup->links) {
        sp_clear_track_setup(setup);
        return false;
    }
    setup->count = count;
    setup->scratch = setup->order + count;
    setup->draft = setup->scratch + count;
    setup->picked = setup->draft + count;
    for (int i = 0; i < count; ++i) {
        setup->order[i] = i;
        setup->links[i] = (SpLink){-1, -1};
    }
    return true;
}

void sp_clear_track_setup(SpTrackSetup *setup)
{
    free(setup->order);
    free(setup->links);
    *setup = (SpTrackSetup){0};
}

static void arrange_tracks(SpTrackSetup *setup, const SpOptions *options)
{
    const SpLink *links = setup->links;
    int first = options->opener;
    int last = sp_sequence_head(links, options->closer);
    int *out = sp_append_sequence(links, first, setup->scratch);
    for (int i = 0; i < setup->count; ++i) {
        int entry = setup->order[i];
        if (links[entry].previous < 0 && entry != first && entry != last)
            out = sp_append_sequence(links, entry, out);
    }
    if (last != first)
        sp_append_sequence(links, last, out);
    memcpy(setup->order, setup->scratch, (size_t)setup->count * sizeof *out);
}

static void add_pick(SpTrackSetup *setup, int entry)
{
    setup->draft[setup->draft_count++] = entry;
    setup->picked[entry] = setup->draft_count;
}

static void remove_pick(SpTrackSetup *setup, int entry)
{
    int index = setup->picked[entry] - 1;
    setup->picked[entry] = 0;
    --setup->draft_count;
    for (int i = index; i < setup->draft_count; ++i) {
        int next = setup->draft[i + 1];
        setup->draft[i] = next;
        setup->picked[next] = i + 1;
    }
}

void sp_start_sequence(SpTrackSetup *setup, int entry)
{
    setup->drafting = true;
    setup->editing = -1;
    const SpLink *links = setup->links;
    if (links[entry].previous < 0 && links[entry].next < 0) {
        add_pick(setup, entry);
        return;
    }
    setup->editing = sp_sequence_head(links, entry);
    for (int item = setup->editing; item >= 0; item = links[item].next)
        add_pick(setup, item);
}

void sp_cancel_sequence(SpTrackSetup *setup)
{
    for (int i = 0; i < setup->draft_count; ++i)
        setup->picked[setup->draft[i]] = 0;
    setup->draft_count = 0;
    setup->editing = -1;
    setup->drafting = false;
}

bool sp_toggle_sequence_track(SpTrackSetup *setup, int entry)
{
    if (setup->picked[entry]) {
        remove_pick(setup, entry);
        return true;
    }
    const SpLink *links = setup->links;
    if (links[entry].previous >= 0 || links[entry].next >= 0) {
        if (sp_sequence_head(links, entry) != setup->editing)
            return false;
    }
    add_pick(setup, entry);
    return true;
}

void sp_unlock_sequence(SpTrackSetup *setup, int entry)
{
    SpLink *links = setup->links;
    for (int item = sp_sequence_head(links, entry); item >= 0;) {
        int next = links[item].next;
        links[item] = (SpLink){-1, -1};
        item = next;
    }
    --setup->sequence_count;
}

static int draft_rank(const SpTrackSetup *setup, int entry)
{
    return entry < 0 ? 0 : setup->picked[entry];
}

static const char *check_draft(const SpTrackSetup *setup, const SpOptions *options)
{
    int start = draft_rank(setup, options->opener);
    int end = draft_rank(setup, options->closer);
    if (setup->draft_count < 2)
        return "Select at least two tracks.";
    if (start > 1)
        return "Put the opener first in the sequence.";
    if (end && end != setup->draft_count)
        return "Put the closer last in the sequence.";
    if (start && end && setup->draft_count < setup->count)
        return "Clear the opener or closer before locking them in the same sequence.";
    return NULL;
}

const char *sp_lock_sequence(SpTrackSetup *setup, const SpOptions *options)
{
    const char *message = check_draft(setup, options);
    if (message)
        return message;
    if (setup->editing >= 0)
        sp_unlock_sequence(setup, setup->editing);
    for (int i = 1; i < setup->draft_count; ++i) {
        int entry = setup->draft[i], previous = setup->draft[i - 1];
        setup->links[previous].next = entry;
        setup->links[entry].previous = previous;
    }
    ++setup->sequence_count;
    sp_cancel_sequence(setup);
    arrange_tracks(setup, options);
    return NULL;
}

bool sp_set_endpoint(SpTrackSetup *setup, SpOptions *options, int entry, bool opening)
{
    int first = sp_sequence_head(setup->links, entry);
    int last = sp_sequence_tail(setup->links, entry);
    int endpoint = opening ? first : last;
    int *value = opening ? &options->opener : &options->closer;
    int other = opening ? options->closer : options->opener;
    if (*value == endpoint) {
        *value = -1;
        return true;
    }
    bool whole_playlist = first == setup->order[0] && last == setup->order[setup->count - 1];
    if (other == (opening ? last : first) && !whole_playlist)
        return false;
    *value = endpoint;
    arrange_tracks(setup, options);
    return true;
}
