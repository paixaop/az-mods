/* SPDX-License-Identifier: GPL-3.0-only
 *
 * anlz.c: read the beat grid, phrases and cues out of a rekordbox ANLZ pair.
 *
 * The format is a tagged container (PMAI header, then 4-byte tags with two length fields).
 * The tags used here:
 *   PQTZ  beat grid, in .DAT
 *   PSSI  song structure / phrases, in .EXT (XOR-masked in rekordbox 6 exports)
 *   PCOB  memory and hot cues
 *   PCO2  extended cues with comments
 *   PPTH  the audio file path the analysis belongs to
 *
 * Everything is validated against the tag lengths. A malformed or partial file yields what
 * could be parsed, never a read outside the buffer.
 */
#include "anlz.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_PQTZ 0x5051545Au
#define TAG_PSSI 0x50535349u
#define TAG_PCOB 0x50434F42u
#define TAG_PCO2 0x50434F32u
#define TAG_PPTH 0x50505448u

#define MAX_BEATS   20000
#define MAX_PHRASES 1024
#define MAX_CUES    256
#define MAX_FILE    (64 * 1024 * 1024)

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int read_file(const char *path, unsigned char **out, long *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (!path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long n = ftell(f);
    if (n <= 0 || n > MAX_FILE) {
        fclose(f);
        return -1;
    }
    rewind(f);
    unsigned char *b = malloc((size_t)n);
    if (!b) {
        fclose(f);
        return -1;
    }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = b;
    *out_len = n;
    return 0;
}

/* UTF-16BE to UTF-8, BMP only. Invalid code units become '?'. */
static void utf16be_to_utf8(const unsigned char *p, int units, char *out, size_t out_n)
{
    size_t o = 0;
    if (!out_n) return;
    for (int i = 0; i < units; i++) {
        unsigned c = be16(p + 2 * i);
        if (c == 0) break;
        unsigned char tmp[4];
        int n;
        if (c < 0x80) {
            tmp[0] = (unsigned char)c;
            n = 1;
        } else if (c < 0x800) {
            tmp[0] = (unsigned char)(0xC0 | (c >> 6));
            tmp[1] = (unsigned char)(0x80 | (c & 0x3F));
            n = 2;
        } else {
            tmp[0] = (unsigned char)(0xE0 | (c >> 12));
            tmp[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
            tmp[2] = (unsigned char)(0x80 | (c & 0x3F));
            n = 3;
        }
        if (o + (size_t)n + 1 > out_n) break;
        memcpy(out + o, tmp, (size_t)n);
        o += (size_t)n;
    }
    out[o] = 0;
}

static int add_beat(an_anlz_t *a, int *cap, uint32_t time, uint16_t tempo, uint8_t bar)
{
    if (a->nbeats >= *cap || a->nbeats >= MAX_BEATS) return -1;
    a->beats[a->nbeats].time_ms = time;
    a->beats[a->nbeats].tempo = tempo;
    a->beats[a->nbeats].beat_in_bar = bar;
    a->nbeats++;
    return 0;
}

static int parse_pqtz(const unsigned char *tag, uint32_t lt, an_anlz_t *a, int *cap)
{
    if (lt < 0x18) return -1;
    uint32_t n = be32(tag + 0x14);
    if (n == 0 || n > MAX_BEATS || 0x18u + n * 8u > lt) return -1;
    if (a->nbeats) return 0; /* first grid wins (the .DAT one) */
    free(a->beats);
    a->beats = calloc(n, sizeof *a->beats);
    if (!a->beats) return -1;
    *cap = (int)n;
    a->has_grid = 1;
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char *e = tag + 0x18 + i * 8u;
        uint16_t num = be16(e + 0);
        uint16_t tempo = be16(e + 2);
        uint32_t time = be32(e + 4);
        if (add_beat(a, cap, time, tempo, (uint8_t)(num ? num : 1)) != 0) break;
    }
    if (a->nbeats > 0 && a->bpm <= 0) {
        /* the first tempo is representative; a later change is handled per beat if needed */
        a->bpm = a->beats[0].tempo / 100.0;
    }
    return a->nbeats > 0 ? 0 : -1;
}

static int parse_pssi(const unsigned char *tag, uint32_t lt, an_anlz_t *a)
{
    if (lt < 0x20) return -1;
    uint32_t entry_bytes = be32(tag + 0x0c);
    uint16_t n = be16(tag + 0x10);
    if (entry_bytes < 24 || entry_bytes > 64) return -1;
    if (n == 0 || n > MAX_PHRASES || 0x20u + (uint32_t)n * entry_bytes > lt) return -1;

    unsigned char *tmp = malloc(lt);
    if (!tmp) return -1;
    memcpy(tmp, tag, lt);

    uint16_t raw_mood = be16(tmp + 0x12);
    if (raw_mood > 20) {
        static const unsigned char base[19] = { 0xCB, 0xE1, 0xEE, 0xFA, 0xE5, 0xEE, 0xAD, 0xEE,
                                                0xE9, 0xD2, 0xE9, 0xEB, 0xE1, 0xE9, 0xF3, 0xE8,
                                                0xE9, 0xF4, 0xE1 };
        unsigned char mask[19];
        for (int i = 0; i < 19; i++) mask[i] = (unsigned char)(base[i] + n);
        for (uint32_t i = 0x12; i < lt; i++) tmp[i] ^= mask[(i - 0x12) % 19];
    }
    a->mood = be16(tmp + 0x12);
    a->end_beat = be16(tmp + 0x1a);

    free(a->phrases);
    a->phrases = calloc(n, sizeof *a->phrases);
    if (!a->phrases) {
        free(tmp);
        return -1;
    }
    a->nphrases = 0;
    for (uint16_t i = 0; i < n; i++) {
        const unsigned char *e = tmp + 0x20 + (uint32_t)i * entry_bytes;
        an_phrase_t ph;
        ph.start_beat = be16(e + 2);
        ph.kind = be16(e + 4);
        ph.end_beat = 0;
        if (ph.start_beat == 0) continue;
        if (i + 1 < n && entry_bytes >= 24) {
            /* the next phrase's start is the reliable boundary */
            const unsigned char *nx = tmp + 0x20 + (uint32_t)(i + 1) * entry_bytes;
            uint16_t nb = be16(nx + 2);
            if (nb > ph.start_beat) ph.end_beat = nb;
        }
        if (ph.end_beat <= ph.start_beat) {
            if (a->end_beat > ph.start_beat)
                ph.end_beat = a->end_beat;
            else
                ph.end_beat = (uint16_t)(ph.start_beat + 1);
        }
        a->phrases[a->nphrases++] = ph;
    }
    free(tmp);
    a->has_phrases = a->nphrases > 0;
    return a->has_phrases ? 0 : -1;
}

static void add_cue(an_anlz_t *a, uint32_t time, uint32_t loop, uint16_t slot,
                    uint8_t kind, const char *label)
{
    if (time == 0 && loop == 0) return;
    for (int i = 0; i < a->ncues; i++) {
        an_cue_t *c = &a->cues[i];
        if (c->slot == slot && c->kind == kind && (c->time_ms > time ? c->time_ms - time : time - c->time_ms) <= 20) {
            if (label && label[0] && strlen(label) > strlen(c->label))
                snprintf(c->label, sizeof c->label, "%s", label);
            if (loop && !c->loop_ms) c->loop_ms = loop;
            return;
        }
    }
    if (!a->cues) a->cues = calloc(MAX_CUES, sizeof *a->cues);
    if (!a->cues || a->ncues >= MAX_CUES) return;
    an_cue_t *c = &a->cues[a->ncues++];
    memset(c, 0, sizeof *c);
    c->time_ms = time;
    c->loop_ms = loop;
    c->slot = slot;
    c->kind = kind;
    if (label) snprintf(c->label, sizeof c->label, "%s", label);
}

static int parse_pcob(const unsigned char *tag, uint32_t lt, an_anlz_t *a)
{
    if (lt < 0x18) return -1;
    uint32_t list_type = be32(tag + 0x0c);
    uint16_t n = be16(tag + 0x12);
    if (!n || n > MAX_CUES) return 0;
    if (0x18u + (uint32_t)n * 0x38u > lt) return -1;
    uint8_t kind = (list_type == 1) ? AN_CUE_HOT : AN_CUE_MEMORY;
    for (uint16_t i = 0; i < n; i++) {
        const unsigned char *e = tag + 0x18 + (uint32_t)i * 0x38u;
        if (memcmp(e, "PCPT", 4) != 0) continue;
        uint32_t slot = be32(e + 0x0c);
        uint32_t time = be32(e + 0x20);
        uint32_t loop = be32(e + 0x24);
        uint8_t type = e[0x1c];
        if (loop == 0xFFFFFFFFu || loop <= time) loop = 0;
        char label[40];
        if (kind == AN_CUE_HOT && slot >= 1 && slot <= 8)
            snprintf(label, sizeof label, "HC%u", slot);
        else
            snprintf(label, sizeof label, "MEM");
        if (loop || type == 2)
            snprintf(label + strlen(label), sizeof label - strlen(label), " LOOP");
        add_cue(a, time, loop, (uint16_t)slot, kind, label);
    }
    return 0;
}

static int parse_pco2(const unsigned char *tag, uint32_t lt, an_anlz_t *a)
{
    if (lt < 0x18) return -1;
    uint32_t list_type = be32(tag + 0x0c);
    uint16_t n = be16(tag + 0x10);
    if (!n || n > MAX_CUES) return 0;
    uint8_t kind = (list_type == 1) ? AN_CUE_HOT : AN_CUE_MEMORY;
    uint32_t off = 0x14;
    for (uint16_t i = 0; i < n; i++) {
        if (off + 0x1cu > lt) break;
        const unsigned char *e = tag + off;
        if (memcmp(e, "PCP2", 4) != 0) break;
        uint32_t len_entry = be32(e + 0x08);
        if (len_entry < 0x1c || off + len_entry > lt) break;
        uint32_t slot = be32(e + 0x0c);
        uint8_t type = e[0x10];
        uint32_t time = be32(e + 0x14);
        uint32_t loop = (len_entry >= 0x1c) ? be32(e + 0x18) : 0;
        if (loop == 0xFFFFFFFFu || loop <= time) loop = 0;
        char label[40] = "";
        uint32_t len_comment = 0;
        if (len_entry > 43 && 0x28u + 4u <= len_entry) {
            len_comment = be32(e + 0x28);
            if (len_comment > 0 && len_comment <= 256 && 0x2cu + len_comment <= len_entry) {
                int units = (int)(len_comment / 2);
                char tmp[160];
                utf16be_to_utf8(e + 0x2c, units, tmp, sizeof tmp);
                snprintf(label, sizeof label, "%s", tmp);
            }
        }
        if (!label[0]) {
            if (kind == AN_CUE_HOT && slot >= 1 && slot <= 8)
                snprintf(label, sizeof label, "HC%u", slot);
            else
                snprintf(label, sizeof label, "MEM");
        }
        if (type == 2) { /* loop entry */
            if (strlen(label) + 5 < sizeof label) strcat(label, " LOOP");
        }
        add_cue(a, time, loop > time ? loop : 0, (uint16_t)slot, kind, label);
        off += len_entry;
    }
    return 0;
}

static void parse_ppth(const unsigned char *tag, uint32_t lt, an_anlz_t *a)
{
    if (a->audio_path[0] || lt < 0x10) return;
    uint32_t len_path = be32(tag + 0x0c);
    if (len_path < 3 || len_path > lt - 0x0c || 0x10u + len_path > lt + 2) return;
    int units = (int)((len_path - 2) / 2);
    if (units <= 0) return;
    utf16be_to_utf8(tag + 0x10, units, a->audio_path, sizeof a->audio_path);
}

static int parse_tags(const unsigned char *buf, long len, an_anlz_t *a, int *cap)
{
    if (!buf || len < 28 || memcmp(buf, "PMAI", 4) != 0) return -1;
    uint32_t header = be32(buf + 4);
    if (header < 28 || header > (uint32_t)len) return -1;
    uint32_t p = header;
    while (p + 12 <= (uint32_t)len) {
        const unsigned char *tag = buf + p;
        uint32_t lt = be32(tag + 8);
        if (lt < 12 || p + lt > (uint32_t)len) break;
        uint32_t fourcc = be32(tag);
        switch (fourcc) {
        case TAG_PQTZ: parse_pqtz(tag, lt, a, cap); break;
        case TAG_PSSI: parse_pssi(tag, lt, a); break;
        case TAG_PCOB: parse_pcob(tag, lt, a); break;
        case TAG_PCO2: parse_pco2(tag, lt, a); break;
        case TAG_PPTH: parse_ppth(tag, lt, a); break;
        default: break;
        }
        p += lt;
    }
    return 0;
}

static int cmp_beat(const void *x, const void *y)
{
    const an_beat_t *a = x, *b = y;
    if (a->time_ms < b->time_ms) return -1;
    if (a->time_ms > b->time_ms) return 1;
    return 0;
}

static int cmp_cue(const void *x, const void *y)
{
    const an_cue_t *a = x, *b = y;
    if (a->time_ms < b->time_ms) return -1;
    if (a->time_ms > b->time_ms) return 1;
    return 0;
}

int an_anlz_load_buffers(const unsigned char *dat, long dat_len,
                         const unsigned char *ext, long ext_len, an_anlz_t *out)
{
    memset(out, 0, sizeof *out);
    int cap = 0;
    if (dat) parse_tags(dat, dat_len, out, &cap);
    if (ext) {
        int cap2 = out->nbeats ? cap : 0;
        parse_tags(ext, ext_len, out, &cap2);
        if (out->nbeats && cap2 > cap) cap = cap2;
    }
    if (out->nbeats > 1) {
        int sorted = 1;
        for (int i = 1; i < out->nbeats; i++)
            if (out->beats[i].time_ms < out->beats[i - 1].time_ms) sorted = 0;
        if (!sorted) qsort(out->beats, (size_t)out->nbeats, sizeof *out->beats, cmp_beat);
        if (out->bpm <= 0) out->bpm = out->beats[0].tempo / 100.0;
    }
    if (out->ncues > 1) qsort(out->cues, (size_t)out->ncues, sizeof *out->cues, cmp_cue);
    out->valid = (out->nbeats > 0 || out->nphrases > 0);
    return out->valid ? 0 : -1;
}

int an_anlz_load(const char *dat_path, const char *ext_path, an_anlz_t *out)
{
    unsigned char *dat = NULL, *ext = NULL;
    long dat_len = 0, ext_len = 0;
    read_file(dat_path, &dat, &dat_len);
    read_file(ext_path, &ext, &ext_len);
    int rc = an_anlz_load_buffers(dat, dat_len, ext, ext_len, out);
    free(dat);
    free(ext);
    return rc;
}

void an_anlz_free(an_anlz_t *a)
{
    if (!a) return;
    free(a->beats);
    free(a->phrases);
    free(a->cues);
    memset(a, 0, sizeof *a);
}

int an_anlz_beat_ms(const an_anlz_t *a, uint32_t beat, uint32_t *ms)
{
    if (!a || !a->nbeats || beat == 0) return -1;
    if (beat <= (uint32_t)a->nbeats) {
        *ms = a->beats[beat - 1].time_ms;
        return 0;
    }
    double bpm = a->bpm > 0 ? a->bpm : 120.0;
    uint32_t last = a->beats[a->nbeats - 1].time_ms;
    *ms = last + (uint32_t)((beat - (uint32_t)a->nbeats) * 60000.0 / bpm);
    return 0;
}

int an_anlz_phrase_start_ms(const an_anlz_t *a, int phrase, uint32_t *ms)
{
    if (!a || phrase < 0 || phrase >= a->nphrases) return -1;
    return an_anlz_beat_ms(a, a->phrases[phrase].start_beat, ms);
}

int an_anlz_phrase_end_ms(const an_anlz_t *a, int phrase, uint32_t *ms)
{
    if (!a || phrase < 0 || phrase >= a->nphrases) return -1;
    uint32_t beat;
    if (phrase + 1 < a->nphrases)
        beat = a->phrases[phrase + 1].start_beat;
    else if (a->end_beat)
        beat = a->end_beat;
    else if (a->nbeats)
        beat = (uint32_t)a->nbeats + 1;
    else
        return -1;
    if (beat <= a->phrases[phrase].start_beat) beat = a->phrases[phrase].start_beat + 1;
    return an_anlz_beat_ms(a, beat, ms);
}

int an_anlz_phrase_at(const an_anlz_t *a, uint32_t pos_ms)
{
    if (!a || !a->nphrases) return -1;
    int found = -1;
    for (int i = 0; i < a->nphrases; i++) {
        uint32_t s;
        if (an_anlz_phrase_start_ms(a, i, &s) != 0) continue;
        if (s > pos_ms) break;
        found = i;
    }
    return found;
}

const char *an_anlz_kind_name(const an_anlz_t *a, uint16_t kind)
{
    if (!a) return "PHRASE";
    if (a->mood == 1) {
        switch (kind) {
        case 1: return "INTRO";
        case 2: return "BUILD";
        case 3: return "BREAK";
        case 5: return "CHORUS";
        case 6: return "OUTRO";
        default: return "PHRASE";
        }
    }
    if (a->mood == 2 || a->mood == 3) {
        switch (kind) {
        case 1: return "INTRO";
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7: return "VERSE";
        case 8: return "BRIDGE";
        case 9: return "CHORUS";
        case 10: return "OUTRO";
        default: return "PHRASE";
        }
    }
    return "PHRASE";
}

int an_anlz_kind_is_chorus(const an_anlz_t *a, uint16_t kind)
{
    if (!a) return 0;
    if (a->mood == 1) return kind == 5;
    if (a->mood == 2 || a->mood == 3) return kind == 9;
    return kind == 5 || kind == 9;
}

double an_anlz_bpm_at(const an_anlz_t *a, uint32_t pos_ms)
{
    if (!a || !a->nbeats) return a ? a->bpm : 0.0;
    int lo = 0, hi = a->nbeats - 1, best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a->beats[mid].time_ms <= pos_ms) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    double bpm = a->beats[best].tempo / 100.0;
    return bpm > 0 ? bpm : a->bpm;
}

uint32_t an_anlz_bar_ms(const an_anlz_t *a, double bpm_fallback)
{
    double bpm = (a && a->bpm > 0) ? a->bpm : bpm_fallback;
    if (bpm < 20.0 || bpm > 400.0) return 0;
    return (uint32_t)(4.0 * 60000.0 / bpm);
}
