/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Host tests for anchor's parser and timing maths. They build small synthetic ANLZ
 * containers, so they need no tracks, no deck and no firmware.
 *
 *   make unit
 *   AN_ANLZ_SAMPLE_DIR=/path/to/anlz-samples make unit    (also parse a real pair)
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anlz.h"
#include "guide.h"
#include "resolve.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef struct {
    unsigned char b[65536];
    long n;
} buf_t;

static void w8(buf_t *b, unsigned v) { b->b[b->n++] = (unsigned char)v; }
static void w16(buf_t *b, unsigned v) { w8(b, v >> 8); w8(b, v & 0xff); }
static void w32(buf_t *b, uint32_t v) { w16(b, v >> 16); w16(b, v & 0xffff); }
static void wmem(buf_t *b, const void *p, size_t n)
{
    memcpy(b->b + b->n, p, n);
    b->n += (long)n;
}
static void wskip(buf_t *b, long n)
{
    memset(b->b + b->n, 0, (size_t)n);
    b->n += n;
}

static void w_tag_start(buf_t *b, const char *fourcc, uint32_t header, uint32_t len)
{
    wmem(b, fourcc, 4);
    w32(b, header);
    w32(b, len);
}

/* A 40-beat, 120 BPM grid; phrase starts at beats 1, 9, 17, 33; the chorus is 17..33. */
static void make_dat(buf_t *b, const char *path)
{
    memset(b, 0, sizeof *b);
    wmem(b, "PMAI", 4);
    w32(b, 28);
    w32(b, 0); /* fixed up below */
    long total_at = b->n - 4;
    wskip(b, 16); /* the rest of the PMAI header */

    /* PPTH */
    uint32_t pathbytes = (uint32_t)(strlen(path) * 2);
    w_tag_start(b, "PPTH", 16, 16 + pathbytes);
    w32(b, pathbytes + 2);
    for (const char *p = path; *p; p++) w16(b, (unsigned char)*p);
    /* no NUL bytes in the tag body: len_path includes them but the sample does not */

    /* PQTZ */
    uint32_t nbeats = 40;
    w_tag_start(b, "PQTZ", 0x18, 0x18 + nbeats * 8);
    w32(b, 0);
    w32(b, 0x80000);
    w32(b, nbeats);
    for (uint32_t i = 0; i < nbeats; i++) {
        w16(b, i % 4 + 1);
        w16(b, 12000);
        w32(b, i * 500);
    }

    /* PCOB: one hot cue at 10 s */
    w_tag_start(b, "PCOB", 0x18, 0x18 + 0x38);
    w32(b, 1); /* hot cue list */
    wskip(b, 2);
    w16(b, 1); /* count */
    w32(b, 0);
    wmem(b, "PCPT", 4);
    w32(b, 0x24);
    w32(b, 0x38);
    w32(b, 1);  /* hot cue 1 */
    w32(b, 1);  /* status */
    w32(b, 0);
    w16(b, 0xffff);
    w16(b, 0xffff);
    w8(b, 1);   /* a cue point */
    wskip(b, 3);
    w32(b, 10000); /* cue time */
    w32(b, 0);
    wskip(b, 16);
    uint32_t total = (uint32_t)b->n;
    memcpy(b->b + total_at, &(unsigned char[4]){ total >> 24, total >> 16, total >> 8, total }, 4);
}

static void make_ext(buf_t *b)
{
    memset(b, 0, sizeof *b);
    wmem(b, "PMAI", 4);
    w32(b, 28);
    w32(b, 0);
    long total_at = b->n - 4;
    wskip(b, 16);

    static const unsigned char base[19] = { 0xCB, 0xE1, 0xEE, 0xFA, 0xE5, 0xEE, 0xAD, 0xEE,
                                            0xE9, 0xD2, 0xE9, 0xEB, 0xE1, 0xE9, 0xF3, 0xE8,
                                            0xE9, 0xF4, 0xE1 };
    static const struct { uint16_t beat, kind; } PHRASES[] = {
        { 1, 1 }, { 9, 2 }, { 17, 5 }, { 25, 5 }, { 33, 6 },
    };
    uint16_t n = 5;
    long tag_at = b->n;
    w_tag_start(b, "PSSI", 0x20, 0x20 + n * 24);
    w32(b, 24);
    w16(b, n);
    /* the payload from here is XOR-masked with base[i] + n */
    w16(b, 1); /* mood high */
    wskip(b, 6);
    w16(b, 40); /* end_beat */
    wskip(b, 4);
    for (int i = 0; i < n; i++) {
        w16(b, (unsigned)(i + 1));
        w16(b, PHRASES[i].beat);
        w16(b, PHRASES[i].kind);
        wskip(b, 18);
    }
    for (long i = tag_at + 0x12; i < b->n; i++)
        b->b[i] ^= (unsigned char)(base[(i - (tag_at + 0x12)) % 19] + n);

    uint32_t total = (uint32_t)b->n;
    memcpy(b->b + total_at, &(unsigned char[4]){ total >> 24, total >> 16, total >> 8, total }, 4);
}

static an_deck_t deck(uint32_t pos, int playing)
{
    an_deck_t d;
    memset(&d, 0, sizeof d);
    d.loaded = 1;
    d.playing = playing;
    d.pos_ms = pos;
    d.bpm = 120.0;
    d.rate = 1.0;
    return d;
}

int main(void)
{
    buf_t dat, ext;
    make_dat(&dat, "/Contents/x/y z/hello.m4a");
    make_ext(&ext);

    an_anlz_t a;
    CHECK(an_anlz_load_buffers(dat.b, dat.n, ext.b, ext.n, &a) == 0);
    CHECK(a.valid && a.has_grid && a.has_phrases);
    CHECK(a.nbeats == 40);
    CHECK(fabs(a.bpm - 120.0) < 0.01);
    CHECK(a.nphrases == 5);
    CHECK(a.mood == 1);
    CHECK(a.end_beat == 40);
    CHECK(a.ncues == 1);
    CHECK(a.cues[0].time_ms == 10000);
    CHECK(strcmp(a.cues[0].label, "HC1") == 0);
    CHECK(strcmp(a.audio_path, "/Contents/x/y z/hello.m4a") == 0);

    uint32_t t = 0;
    CHECK(an_anlz_phrase_start_ms(&a, 2, &t) == 0 && t == 8000);
    CHECK(an_anlz_phrase_end_ms(&a, 2, &t) == 0 && t == 12000);
    CHECK(an_anlz_phrase_end_ms(&a, 3, &t) == 0 && t == 16000);
    CHECK(an_anlz_phrase_at(&a, 9000) == 2);
    CHECK(an_anlz_phrase_at(&a, 1000) == 0);
    CHECK(an_anlz_kind_is_chorus(&a, 5) == 1);
    CHECK(strcmp(an_anlz_kind_name(&a, 5), "CHORUS") == 0);
    CHECK(fabs(an_anlz_bpm_at(&a, 0) - 120.0) < 0.01);
    CHECK(fabs(an_anlz_bpm_at(&a, 9000) - 120.0) < 0.01);

    an_target_opts_t o = { AN_RULE_PHRASE, AN_CAT_CHORUS, AN_POINT_END, 1, 1, 1 };
    an_cand_t c;
    an_group_t groups[16];
    CHECK(an_phrase_groups(&a, 1, groups, 16) == 4); /* the two choruses join */
    CHECK(an_phrase_groups(&a, 0, groups, 16) == 5);
    CHECK(an_phrase_groups(&a, 1, groups, 16) == 4 && groups[2].start_ms == 8000 &&
          groups[2].end_ms == 16000);
    CHECK(an_auto_target(&a, 9000, &o, &c) == 0);
    CHECK(c.ms == 16000);
    CHECK(strcmp(c.label, "CHORUS END") == 0);
    o.consolidate = 0;
    CHECK(an_auto_target(&a, 9000, &o, &c) == 0 && c.ms == 12000);
    o.consolidate = 1;
    o.phrase_point = AN_POINT_START;
    CHECK(an_auto_target(&a, 1000, &o, &c) == 0 && c.ms == 8000);
    CHECK(strcmp(c.label, "CHORUS START") == 0);
    o.rule = AN_RULE_HOTCUE;
    o.hotcue_any = 1;
    CHECK(an_auto_target(&a, 1000, &o, &c) == 0 && c.ms == 10000);
    CHECK(strcmp(c.label, "HC1") == 0);
    o.hotcue_any = 0;
    o.hotcue_slot = 1;
    CHECK(an_auto_target(&a, 1000, &o, &c) == 0 && c.ms == 10000);
    o.hotcue_slot = 2;
    CHECK(an_auto_target(&a, 1000, &o, &c) != 0);
    CHECK(an_find_drop(&a, 0, &c) == 0 && c.ms == 8000);

    o.rule = AN_RULE_PHRASE;
    o.phrase_cat = -1; /* the default: the section playing now */
    o.phrase_point = AN_POINT_END;
    o.consolidate = 1;
    CHECK(an_auto_target(&a, 9000, &o, &c) == 0 && c.ms == 16000);
    o.consolidate = 0;
    CHECK(an_auto_target(&a, 9000, &o, &c) == 0 && c.ms == 12000);
    o.phrase_cat = AN_CAT_INTRO; /* no intro start ahead: fall back to the next phrase start */
    o.phrase_point = AN_POINT_START;
    o.consolidate = 1;
    CHECK(an_auto_target(&a, 1000, &o, &c) == 0 && c.ms == 4000);
    CHECK(strcmp(c.label, "BUILD START") == 0);
    o.phrase_cat = AN_CAT_CHORUS;
    o.phrase_point = AN_POINT_END;
    an_cand_t cands[14];
    int cn = an_candidates(&a, 1000, &o, cands, 14);
    CHECK(cn >= 4);
    CHECK(cands[0].ms <= cands[cn - 1].ms);
    int saw_cue = 0, saw_chorus_end = 0;
    for (int i = 0; i < cn; i++) {
        if (cands[i].ms == 10000) saw_cue = 1;
        if (cands[i].ms == 16000) saw_chorus_end = 1;
    }
    CHECK(saw_cue && saw_chorus_end);

    /* A target 16 s; B paused at 0, B drop 8 s: play at 8 s. */
    an_deck_t A = deck(1000, 1), B = deck(0, 0);
    an_loop_t loop;
    memset(&loop, 0, sizeof loop);
    an_plan_t p;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 0, &p);
    CHECK(p.state == AN_ST_SINGLE);
    CHECK(p.action == 1);
    CHECK(fabs((double)p.action_wall_ms - 7000.0) < 20.0);

    /* Countdown -> exact PLAY NOW -> missed. Never cue before the deadline; retain the
     * cue briefly after it so a UI tick can actually paint it. */
    A.pos_ms = 7900;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 6900, &p);
    CHECK(p.state == AN_ST_SINGLE && !p.play_now && p.bars_action > 0);
    A.pos_ms = 8000;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 7000, &p);
    CHECK(p.state == AN_ST_SINGLE && p.play_now && p.accent == 4);
    A.pos_ms = 8100;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 7100, &p);
    CHECK(p.state == AN_ST_SINGLE && p.play_now);
    A.pos_ms = 8200;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 7200, &p);
    CHECK(p.state == AN_ST_MISSED && !p.play_now);
    A = deck(1000, 1);
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", UINT64_C(5000000000), &p);
    CHECK(p.action_wall_ms == UINT64_C(5000007000));

    /* 4-bar loop out at the drop: the start window is the same 7 s, with one loop. */
    loop.valid = 1;
    loop.in_ms = 0;
    loop.out_ms = 8000;
    loop.len_ms = 8000;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 0, &p);
    CHECK(p.state == AN_ST_START);
    CHECK(p.loops == 1);

    /* A paused loop must not jump to its next start slot before showing PLAY NOW. */
    A.pos_ms = 7900;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 6900, &p);
    CHECK(p.state == AN_ST_START && !p.play_now && p.bars_action > 0);
    A.pos_ms = 8000;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 7000, &p);
    CHECK(p.state == AN_ST_START && p.play_now);
    A.pos_ms = 8100;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 7100, &p);
    CHECK(p.state == AN_ST_START && p.play_now);
    A.pos_ms = 8200;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 7200, &p);
    CHECK(p.state == AN_ST_MISSED && !p.play_now);
    A = deck(1000, 1);

    /* B looping at 1 s: the aligned pass ends at 15 s, so its window opens at 7 s. */
    B = deck(1000, 1);
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 0, &p);
    CHECK(p.state == AN_ST_WAIT);
    CHECK(p.window_open == 0);
    CHECK(fabs(p.bars_action - 3.5) < 0.6); /* 7 s at 120 BPM = 3.5 bars */

    /* At 7 s the pass that ends at 14 s is the aligned one, and its window is open. */
    A.pos_ms = 1000;
    B = deck(1000, 1);
    an_plan(&A, &a, &B, &a, &loop, 8000, "CHORUS END", 7000, &p);
    CHECK(p.state == AN_ST_OPEN);
    CHECK(p.window_open == 1);

    /* EXIT NOW must not appear in the preceding pass, even 25 ms before loop-out. */
    A = deck(7975, 1);
    B = deck(7975, 1);
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 0, &p);
    CHECK(p.state == AN_ST_WAIT && !p.window_open && p.bars_action > 0);
    A = deck(8000, 1);
    B = deck(0, 1);
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 25, &p);
    CHECK(p.state == AN_ST_OPEN && p.window_open);

    /* A loop after the drop cannot be used. */
    loop.out_ms = 20000;
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 0, &p);
    CHECK(p.state == AN_ST_OFF);

    /* No time left for the single start: a missed target. */
    memset(&loop, 0, sizeof loop);
    A = deck(9000, 1);
    B = deck(0, 0);
    an_plan(&A, &a, &B, &a, &loop, 12000, "CHORUS END", 0, &p);
    CHECK(p.state == AN_ST_MISSED);

    /* The loop was released and the drop is being waited for. */
    memset(&loop, 0, sizeof loop);
    loop.released = 1;
    A = deck(1000, 1);
    B = deck(8500, 1);
    an_plan(&A, &a, &B, &a, &loop, 16000, "CHORUS END", 0, &p);
    CHECK(p.state == AN_ST_RELEASED);

    an_anlz_free(&a);

    /* The analysis-folder hash, checked against a real export.pdb row
     * (/Contents/Evokings/... -> /PIONEER/USBANLZ/P056/0001D256/). */
    char folder[64];
    an_anlz_folder_hash("/Contents/Evokings/Losing My Religion (Remix)/Evokings - Losing My Religion (Extended Mix).mp3",
                        folder, sizeof folder);
    CHECK(strcmp(folder, "P056/0001D256") == 0);

    /* Optional: the real sample pair from a deck, when the caller has one. */
    const char *dir = getenv("AN_ANLZ_SAMPLE_DIR");
    if (dir && dir[0]) {
        char datp[1024], extp[1024];
        snprintf(datp, sizeof datp, "%s/ANLZ0000.DAT", dir);
        snprintf(extp, sizeof extp, "%s/ANLZ0000.EXT", dir);
        an_anlz_t s;
        CHECK(an_anlz_load(datp, extp, &s) == 0);
        CHECK(s.nbeats > 100 && s.nphrases > 4 && s.ncues > 0);
        CHECK(s.bpm > 60 && s.bpm < 200);
        printf("real sample: %d beats, %d phrases, %d cues, %.2f BPM\n", s.nbeats, s.nphrases,
               s.ncues, s.bpm);
        an_anlz_free(&s);
    }

    printf("all anchor host tests passed\n");
    return 0;
}
