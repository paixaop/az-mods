/* SPDX-License-Identifier: GPL-3.0-only
 *
 * anlz.h: the parts of a rekordbox ANLZ analysis file the guide needs.
 *
 * The deck parses these files when it loads a track; the guide normally reads that copy
 * (loaded.h) and uses this parser for the cue list it does not carry, and as the fallback
 * on firmware whose loaded-state layout is not verified. Read-only.
 *
 * All times are the file's own times, in milliseconds at 100 % pitch. The caller converts
 * them to wall time with the deck's current playback rate.
 */
#ifndef ANCHOR_ANLZ_H
#define ANCHOR_ANLZ_H

#include <stdint.h>

typedef struct {
    uint32_t time_ms;   /* beat time at 100 % pitch */
    uint16_t tempo;     /* BPM x 100 at this beat */
    uint8_t  beat_in_bar; /* 1 = downbeat */
} an_beat_t;

typedef struct {
    uint16_t start_beat; /* 1-based absolute beat number */
    uint16_t end_beat;   /* 1-based; first beat after the phrase (or the file's end beat) */
    uint16_t kind;       /* mood-specific phrase kind */
} an_phrase_t;

enum { AN_CUE_MEMORY = 0, AN_CUE_HOT = 1 };

typedef struct {
    uint32_t time_ms;
    uint32_t loop_ms;   /* 0 = a point, otherwise the loop out time */
    uint16_t slot;      /* 1..8 for hot cues, 0 for memory cues */
    uint8_t  kind;      /* AN_CUE_* */
    char     label[40];
} an_cue_t;

typedef struct {
    int      valid;
    int      has_grid;
    int      has_phrases;
    uint16_t mood;          /* 1 high, 2 mid, 3 low, 0 unknown */
    double   bpm;           /* from the grid, 0 if unknown */
    uint32_t end_beat;      /* PSSI's end_beat: the last phrase's end beat */
    an_beat_t   *beats;
    int          nbeats;
    an_phrase_t *phrases;
    int          nphrases;
    an_cue_t    *cues;
    int          ncues;
    char         audio_path[512]; /* PPTH */
} an_anlz_t;

/* Parse a pair of ANLZ files. Either path may be NULL when that file is missing. Returns 0
 * when at least a grid or a phrase list was parsed. */
int an_anlz_load(const char *dat_path, const char *ext_path, an_anlz_t *out);
int an_anlz_load_buffers(const unsigned char *dat, long dat_len,
                         const unsigned char *ext, long ext_len, an_anlz_t *out);
void an_anlz_free(an_anlz_t *a);

/* Time of a 1-based beat number, in file milliseconds. Returns 0, or -1 if unknown. */
int an_anlz_beat_ms(const an_anlz_t *a, uint32_t beat, uint32_t *ms);
/* Start time of a phrase, in file milliseconds. */
int an_anlz_phrase_start_ms(const an_anlz_t *a, int phrase, uint32_t *ms);
/* End (boundary) time of a phrase: the start time of the next phrase, or PSSI's end_beat. */
int an_anlz_phrase_end_ms(const an_anlz_t *a, int phrase, uint32_t *ms);
/* Index of the phrase playing at pos_ms, or -1. */
int an_anlz_phrase_at(const an_anlz_t *a, uint32_t pos_ms);
/* Long form kind name for a phrase ("CHORUS", "VERSE", "BUILD" ...). */
const char *an_anlz_kind_name(const an_anlz_t *a, uint16_t kind);
/* 1 when the phrase is a chorus/drop in this mood. */
int an_anlz_kind_is_chorus(const an_anlz_t *a, uint16_t kind);
/* BPM of the grid beat nearest before pos (handles tempo changes in the analysis). */
double an_anlz_bpm_at(const an_anlz_t *a, uint32_t pos_ms);
/* Milliseconds per bar from the grid, or from a fallback bpm. */
uint32_t an_anlz_bar_ms(const an_anlz_t *a, double bpm_fallback);

#endif
