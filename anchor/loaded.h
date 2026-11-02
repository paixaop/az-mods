/* SPDX-License-Identifier: GPL-3.0-only
 *
 * loaded.h: the analysis the deck already has for a loaded track.
 *
 * The XDJ-AZ parses a track's rekordbox analysis when it loads it: its beat grid, its
 * phrase map and the phrase meter's sections all live in EP147's memory. Reading that copy
 * is milliseconds of memory reads, where walking the USB stick for ANLZ0000.DAT can take
 * minutes on a large library.
 *
 * This module reads the copy through /proc/<pid>/mem: it never dereferences a pointer in
 * our own process, so a replaced or freed analysis gives a short read (an error), not a
 * crash in EP147. Every pointer, vtable and count is checked before use, and the whole
 * read is bounded. It is verified against EP147 2.00 (the firmware tag in
 * ANCHOR_LOADED_FW); on any other build the caller keeps using the ANLZ files.
 */
#ifndef ANCHOR_LOADED_H
#define ANCHOR_LOADED_H

#include <stddef.h>
#include <stdint.h>

#include "anlz.h"

#define ANCHOR_LOADED_FW "ep147-9e9454e357389ece"
/* The native UI has not created this deck's LoadState yet, or replaced its snapshot.
 * Retry on the worker; this does not mean the track needs rekordbox analysis. */
#define AN_LOADED_RETRY (-2)

/* 0 = this process, else the pid to read (for the host test tool). Call before the first
 * an_loaded_read; returns 0 when the memory file opened. */
int an_loaded_open(int pid);
void an_loaded_close(void);

/* 1 when the loaded-state reader found the four LoadState objects. */
int an_loaded_ready(void);

/* Fill `out` from the deck's loaded analysis. 0 = a usable grid (and optional phrases);
 * AN_LOADED_RETRY = native state not ready/replaced; -1 = unusable analysis. On failure
 * out is freed/empty and `why` describes the read failure.
 * Grid/phrase providers must belong to the same stable snapshot. Phrase beat numbers
 * without a grid are rejected. out->has_phrases is 0 for a grid-only track. */
int an_loaded_read(int deck, an_anlz_t *out, char *why, size_t why_n);

typedef struct {
    int available, engaged;
    uint32_t in_ms, out_ms;
} an_loaded_loop_t;
/* Worker only, on ANCHOR_LOADED_FW. Reads the current working loop, never saved slots.
 * A successful inactive result is distinct from unavailable/unstable native state. */
int an_loaded_loop_read(int deck, an_loaded_loop_t *out);

#endif
