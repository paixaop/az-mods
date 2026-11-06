/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ANCHOR_DECK_H
#define ANCHOR_DECK_H

#include "azmod-mod.h"
#include "guide.h"

#define AN_DECKS 2

enum { AN_DECK_UPDATED = 1, AN_DECK_REPLACED = 2, AN_DECK_ANALYSIS = 4 };

typedef struct {
    uint64_t epoch, head_ns;
    int loaded, playback, direction, position_valid;
    uint32_t pos_ms, sample_rate;
    double rate, bpm, original_bpm;
    int loop_availability, loop_engaged;
    an_loop_t loop;
    int grid_pending, phrases_pending, hot_cues, memory_cues;
    an_analysis_t analysis;
} an_deck_status_t;

/* Replace the cached view from an INITIAL, CHANGE or RESYNC. Collections are
 * copied into mod-owned buffers only on analysis publications. UI thread only. */
unsigned an_deck_update(an_deck_status_t decks[AN_DECKS], const az_deck_status_event_v1 *event);

int an_availability_pending(int availability);

#endif
