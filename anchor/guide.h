/* SPDX-License-Identifier: GPL-3.0-only
 *
 * guide.h: the transition arithmetic, without any deck or core services.
 *
 * The mod reads a snapshot of each deck plus its ANLZ analysis and asks this module what to
 * show. Everything here is pure: times in, one plan out. The host tests exercise it directly.
 */
#ifndef ANCHOR_GUIDE_H
#define ANCHOR_GUIDE_H

#include <stdint.h>

#include "anlz.h"

typedef struct {
    int      loaded;
    int      playing;
    uint32_t pos_ms;   /* audio playhead, at 100 % pitch rate */
    double   bpm;      /* the deck's current BPM */
    double   rate;     /* audio ms per wall ms: 1.0 = normal pitch */
} an_deck_t;

typedef struct {
    int      valid;     /* a loop the guide can use */
    int      released;  /* the DJ left the loop; the drop is on its way */
    uint32_t in_ms;
    uint32_t out_ms;
    uint32_t len_ms;
} an_loop_t;

typedef struct {
    uint32_t ms;
    char     label[40];
    int      chorus;
} an_cand_t;

enum {
    AN_RULE_PHRASE = 0,
    AN_RULE_HOTCUE = 1,
    AN_RULE_GRID16 = 2,
    AN_RULE_GRID32 = 3,
};

enum {
    AN_CAT_OTHER = 0,
    AN_CAT_INTRO,
    AN_CAT_BUILD,
    AN_CAT_BREAK,
    AN_CAT_CHORUS,
    AN_CAT_OUTRO,
    AN_CAT_VERSE,
    AN_CAT_BRIDGE,
};

enum { AN_POINT_END = 0, AN_POINT_START = 1 };

typedef struct {
    int rule;          /* AN_RULE_* */
    int phrase_cat;    /* -1 = any, else AN_CAT_* */
    int phrase_point;  /* AN_POINT_* */
    int consolidate;   /* join contiguous phrases of the same category */
    int hotcue_any;    /* 1 = the next hot cue, 0 = hotcue_slot */
    int hotcue_slot;   /* 1..8 */
} an_target_opts_t;

/* A phrase, or a run of contiguous phrases of the same category when consolidation is on. */
typedef struct {
    int      first, last;  /* phrase indexes */
    uint16_t kind;         /* kind of the first phrase */
    int      category;     /* AN_CAT_* */
    uint32_t start_ms, end_ms;
} an_group_t;

enum {
    AN_ST_NONE = 0,
    AN_ST_NO_ANALYSIS,
    AN_ST_NEED_TRACKS,
    AN_ST_SINGLE,    /* B paused, no loop: one moment to press play */
    AN_ST_START,     /* B paused in a loop: the next valid start window */
    AN_ST_WAIT,      /* B looping, the release window is ahead */
    AN_ST_OPEN,      /* the release window is open now */
    AN_ST_RELEASED,  /* the loop was released; the drop is being waited for */
    AN_ST_OFF,       /* the loop phase does not line up with the target */
    AN_ST_PLAYING,   /* B playing without a loop */
    AN_ST_MISSED,    /* the window passed */
};

enum {
    AN_NOTICE_NONE = 0,
    AN_NOTICE_READING,
    AN_NOTICE_ANALYSIS_A,
    AN_NOTICE_ANALYSIS_B,
    AN_NOTICE_NO_CUES,
    AN_NOTICE_NO_DROP,
    AN_NOTICE_LOOP_UNKNOWN,
    AN_NOTICE_LOOP_AFTER_DROP,
    AN_NOTICE_HEAD_PAST_LOOP,
    AN_NOTICE_LOOP_DETECTING,
    AN_NOTICE_START_OUTGOING,
    AN_NOTICE_B_TARGET_PASSED,
};

/* Keep the cue visible across UI ticks without showing PLAY NOW before its deadline. */
#define AN_PLAY_GRACE_MS 150.0

typedef struct {
    int      state;
    int      valid;
    int      action;       /* 1 = press play, 2 = release the loop, 0 = wait */
    int      window_open;
    int      accent;       /* 0 dim, 1 normal, 2 amber, 3 red, 4 green */
    int      notice;       /* structured reason for the action-first display */
    int      play_now;
    int      tempo_mismatch;
    int      drop_aligned;
    int      looping;      /* incoming deck is playing inside a confirmed loop */
    int      loop_prepared; /* engaged loop with B paused */
    char     target[48];
    char     b_target[40];
    uint32_t b_target_ms;
    int      b_target_valid;
    char     line1[96];
    char     line2[96];
    double   bars_action;  /* countdown to action, in A bars; < 0 = past */
    uint32_t target_ms;
    uint64_t action_wall_ms;
    uint64_t out_wall_ms;
    int      loops;        /* loops that will play before the release */
} an_plan_t;

/* Phrase groups for target selection: raw phrases, or contiguous same-category runs. */
int an_phrase_groups(const an_anlz_t *a, int consolidate, an_group_t *out, int max);
/* The category of a phrase kind in this file's mood. */
int an_phrase_category(const an_anlz_t *a, uint16_t kind);
const char *an_category_name(int category);

/* Upcoming phrase boundaries, hot cues and (for the grid rules) bar boundaries, sorted by
 * time. Each label is short enough for the strip. */
int an_candidates(const an_anlz_t *a, uint32_t pos_ms, const an_target_opts_t *opts,
                  an_cand_t *out, int max);
/* The target the settings pick without the DJ choosing one. */
int an_auto_target(const an_anlz_t *a, uint32_t pos_ms, const an_target_opts_t *opts,
                   an_cand_t *out);
/* The first chorus (or first phrase) at/after B's head: where B's drop starts. */
int an_find_drop(const an_anlz_t *b, uint32_t pos_ms, an_cand_t *out);
/* B destinations span the whole track, independent of head/loop position. */
int an_b_candidates(const an_anlz_t *b, int hot_cues, an_cand_t *out, int max);
int an_suggest_phrase(const an_anlz_t *b, uint32_t pos_ms, an_cand_t *out);
/* Milliseconds per bar for display. */
uint32_t an_bar_ms(const an_deck_t *d, const an_anlz_t *a);

/* The plan. `target` is what the DJ latched, or the auto target. now_ms is CLOCK_MONOTONIC
 * milliseconds. `released` is set by the caller when it saw B leave a loop. */
void an_plan(const an_deck_t *a, const an_anlz_t *aa, const an_deck_t *b,
             const an_anlz_t *ba, const an_loop_t *loop, uint32_t target_ms,
             const char *target_label, uint64_t now_ms, an_plan_t *out);
void an_plan_target(const an_deck_t *a, const an_anlz_t *aa, const an_deck_t *b,
                    const an_anlz_t *ba, const an_loop_t *loop, uint32_t target_ms,
                    const char *target_label, const an_cand_t *b_target,
                    uint64_t now_ms, an_plan_t *out);

#endif
