/* SPDX-License-Identifier: GPL-3.0-only
 *
 * loaded.c: read the analysis EP147 already parsed for a loaded track.
 *
 * Verified against EP147 2.00 (vtable 0x318ec68, LoadState): the four LoadState objects are
 * one array, deck at +0x47c, magic 0x5e715e28 at +0x4a8. Each has:
 *   +0x170  gui::AnalyzedDataProvider<BeatGrid>  (embedded)
 *     +0x10 -> meow::UpdatableSharedObject<BeatGrid> (vptr 0x2a9cfa8)
 *       +0x28 -> BeatGrid::Content (vptr 0x2ae4df8)
 *                  +0x28 Cell[] { int64 pos; double bpm }, +0x30 nbeats
 *   +0x3d8  gui::TrackStructureData (vptr 0x318ea38, second base +0x10 = 0x318ea68)
 *     +0x60 -> meow::UpdatableSharedObject<TrackPhrase> (vptr 0x2c99430)
 *       +0x28 -> TrackPhrase::Content (vptr 0x2ae4800)
 *                  +0x38 count, +0x3c genre, +0x40 Section[], +0x48 count2
 *     +0x68 -> the same UpdatableSharedObject<BeatGrid> (cross-check)
 *
 * A Section is 0x14 bytes: int kind, int start_beat (1-based), int end_beat, u16 k1, u16 k2,
 * u16 k3. The kind is rekordbox's PSSI kind; its meaning depends on the genre field
 * (Content+0x3c): 0 = high mood, 1 = mid, 2 = low. Phrase starts were checked beat for beat,
 * and kind for kind, against the track's ANLZ0000.EXT on the deck.
 *
 * Beats are grid cells; cell pos is source samples at the grid's own rate, which is measured
 * from the cells (44100 on every grid seen). The ms values this module produces therefore
 * match the ANLZ file's own ms times, which are at 100 % pitch.
 *
 * Reading is through /proc/<pid>/mem with pread: a stale pointer gives EIO, never a SIGSEGV
 * in EP147. Every object is vtable-checked, every count is bounded, and the phrase/grid
 * pointers are re-read after the copy to catch a track change mid-read.
 */
#include "loaded.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#define LS_STRIDE 0x500UL
#define LS_DECK 0x47cUL
#define LS_MAGIC_OFF 0x4a8UL
#define LS_MAGIC 0x5e715e28u
#define LS_TSD 0x3d8UL
#define LS_PROV 0x170UL

#define VT_LOADSTATE 0x318ec68UL
#define VT_TSD 0x318ea38UL
#define VT_USO_GRID 0x2a9cfa8UL
#define VT_USO_PHRASE 0x2c99430UL
#define VT_CONTENT_GRID 0x2ae4df8UL
#define VT_CONTENT_PHRASE 0x2ae4800UL

#define USO_CONTENT 0x28UL
#define GRID_CELLS 0x28UL
#define GRID_NBEATS 0x30UL
#define PH_COUNT 0x38UL
#define PH_GENRE 0x3cUL
#define PH_SECTIONS 0x40UL
#define PH_COUNT2 0x48UL

#define MAX_PHRASES 1024
#define MAX_BEATS 20000
#define MAX_SECTION_BYTES (MAX_PHRASES * 0x14)
#define SCAN_CHUNK (1u << 20)
#define SCAN_MAX_BYTES (768u << 20)

typedef struct {
    uint64_t obj;
    unsigned used;
} ls_t;

static int g_fd = -1;
static int g_pid;
static ls_t g_ls[4];
static int g_ready;
static int g_scanned;
static uint64_t g_loop_units[4];
static int g_loop_scanned;
static uint64_t g_loop_scan_after;

/* EP147 2.00: DjPlayerUnit -> UnitStateServer (+0x1a0), logical deck at +0x1f18.
 * Its PlayerState base is +8, whose current working CueProperty is +0xb18.
 * Retained/saved cue slots are separate. See docs/PREPARED-LOOPS.md for RE/runtime evidence. */
#define VT_LOOP_UNIT UINT64_C(0x2bf2348)
#define VT_LOOP_PUBLISHER UINT64_C(0x2bf49b8)
#define VT_LOOP_PLAYER_BASE UINT64_C(0x2bf4a08)
#define VT_LOOP_PROPERTY UINT64_C(0x2be12b0)
#define VT_LOOP_POINT UINT64_C(0x29249b0)
#define VT_LOOP_POSITION UINT64_C(0x2924998)
#define LOOP_PUBLISHER 0x1a0u
#define LOOP_DECK 0x1f18u
#define LOOP_WORKING 0xb20u

static int rd(uint64_t a, void *buf, size_t n)
{
    if (g_fd < 0) return -1;
    ssize_t r = pread(g_fd, buf, n, (off_t)a);
    return r == (ssize_t)n ? 0 : -1;
}

static uint64_t q(uint64_t a)
{
    uint64_t v = 0;
    return rd(a, &v, 8) == 0 ? v : 0;
}

static uint32_t u32(uint64_t a)
{
    uint32_t v = 0;
    return rd(a, &v, 4) == 0 ? v : 0;
}

static int32_t i32(uint64_t a)
{
    int32_t v = 0;
    return rd(a, &v, 4) == 0 ? v : 0;
}

static int is_map_anon(const char *line, char *perms, unsigned long long *lo,
                       unsigned long long *hi)
{
    char dev[32], path[512];
    unsigned long ino, off;
    path[0] = 0;
    int n = sscanf(line, "%llx-%llx %7s %lx %31s %lu %511[^\n]", lo, hi, perms, &off, dev, &ino,
                   path);
    if (n < 6) return 0;
    if (path[0]) {
        /* strip leading blanks that the scanset keeps */
        char *p = path;
        while (*p == ' ' || *p == '\t') p++;
        if (*p) return 0;
    }
    return perms[0] == 'r' && perms[1] == 'w';
}

/* Record a LoadState object if the address holds one. */
static void note_ls(uint64_t obj, uint64_t lo, uint64_t hi)
{
    if (obj < lo || obj + LS_MAGIC_OFF + 4 > hi) return;
    if (u32(obj + LS_MAGIC_OFF) != LS_MAGIC) return;
    if (q(obj) != VT_LOADSTATE) return;
    uint32_t deck = u32(obj + LS_DECK);
    if (deck > 3) return;
    if (!g_ls[deck].used || g_ls[deck].obj != obj) {
        g_ls[deck].obj = obj;
        g_ls[deck].used = 1;
    }
}

static void scan_window(uint64_t lo, uint64_t hi)
{
    size_t n = (size_t)(hi - lo);
    if (n < 0x4ac || n > (4u << 20)) return;
    unsigned char *buf = malloc(n);
    if (!buf) return;
    if (rd(lo, buf, n) != 0) {
        free(buf);
        return;
    }
    for (size_t off = 0; off + 0x4ac <= n; off += 8) {
        uint64_t v;
        memcpy(&v, buf + off, 8);
        if (v == VT_LOADSTATE) note_ls(lo + off, lo, hi);
    }
    free(buf);
}

/* Find the LoadState array once. The array lives in a large anonymous heap mapping, so
 * the mappings are scanned largest first; after one object is found the other decks are
 * looked for in a small window around it (they are one 0x500-byte stride apart). */
static int find_ls(void)
{
    if (g_scanned) return g_ready;
    memset(g_ls, 0, sizeof g_ls);
    g_ready = 0;
    g_scanned = 1;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", g_pid);
    FILE *f = fopen(path, "r");
    if (!f) { g_scanned = 0; return 0; }

    typedef struct {
        uint64_t lo, hi;
    } map_t;
    map_t maps[256];
    int nm = 0;
    char line[640];
    while (fgets(line, sizeof line, f) && nm < 256) {
        char perms[8];
        unsigned long long lo, hi;
        if (!is_map_anon(line, perms, &lo, &hi)) continue;
        if (hi <= lo) continue;
        maps[nm].lo = lo;
        maps[nm].hi = hi;
        nm++;
    }
    fclose(f);
    /* largest first: insertion sort, 256 entries at most */
    for (int i = 1; i < nm; i++) {
        map_t m = maps[i];
        int j = i - 1;
        while (j >= 0 && (maps[j].hi - maps[j].lo) < (m.hi - m.lo)) {
            maps[j + 1] = maps[j];
            j--;
        }
        maps[j + 1] = m;
    }

    unsigned long long scanned = 0;
    unsigned char *buf = malloc(SCAN_CHUNK);
    if (!buf) return 0;
    for (int i = 0; i < nm && scanned < SCAN_MAX_BYTES; i++) {
        uint64_t lo = maps[i].lo, hi = maps[i].hi;
        for (uint64_t a = lo; a < hi && scanned < SCAN_MAX_BYTES; a += SCAN_CHUNK) {
            size_t want = (size_t)(hi - a < SCAN_CHUNK ? hi - a : SCAN_CHUNK);
            if (rd(a, buf, want) != 0) {
                /* a hole in the mapping: page at a time */
                for (size_t o = 0; o < want; o += 4096) {
                    size_t w = want - o < 4096 ? want - o : 4096;
                    if (rd(a + o, buf + o, w) != 0) memset(buf + o, 0, w);
                }
            }
            scanned += want;
            for (size_t o = 0; o + 8 <= want; o += 8) {
                uint64_t v;
                memcpy(&v, buf + o, 8);
                if (v != VT_LOADSTATE) continue;
                if (o + LS_MAGIC_OFF + 4 <= want) {
                    uint32_t m;
                    memcpy(&m, buf + o + LS_MAGIC_OFF, 4);
                    if (m != LS_MAGIC) continue;
                    uint32_t deck;
                    memcpy(&deck, buf + o + LS_DECK, 4);
                    if (deck <= 3) {
                        g_ls[deck].obj = a + o;
                        g_ls[deck].used = 1;
                    }
                } else {
                    note_ls(a + o, lo, hi);
                }
            }
            if (g_ls[0].used || g_ls[1].used || g_ls[2].used || g_ls[3].used) break;
        }
        if (g_ls[0].used || g_ls[1].used || g_ls[2].used || g_ls[3].used) break;
    }
    free(buf);

    if (g_ls[0].used || g_ls[1].used || g_ls[2].used || g_ls[3].used) {
        /* the four are contiguous; read a window around the first one */
        uint64_t base = 0;
        for (int d = 0; d < 4; d++)
            if (g_ls[d].used) {
                base = g_ls[d].obj;
                break;
            }
        uint64_t lo = base > 0x8000 ? base - 0x8000 : 0, hi = base + 0x8000;
        for (int i = 0; i < nm; i++) if (base >= maps[i].lo && base < maps[i].hi) {
            if (lo < maps[i].lo) lo = maps[i].lo;
            if (hi > maps[i].hi) hi = maps[i].hi;
            break;
        }
        scan_window(lo, hi);
        g_ready = 1;
    } else {
        g_scanned = 0; /* not there yet (an early call): look again next time */
    }
    return g_ready;
}

int an_loaded_open(int pid)
{
    int want = pid > 0 ? pid : (int)getpid();
    if (g_fd >= 0 && g_pid == want) return 0; /* already open: keep the worker's fd */
    an_loaded_close();
    g_pid = want;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/mem", g_pid);
    g_fd = open(path, O_RDONLY | O_CLOEXEC);
    return g_fd >= 0 ? 0 : -1;
}

void an_loaded_close(void)
{
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    memset(g_ls, 0, sizeof g_ls);
    g_ready = 0;
    g_scanned = 0;
    g_loop_scanned = 0;
    g_loop_scan_after = 0;
    memset(g_loop_units,0,sizeof g_loop_units);
}

int an_loaded_ready(void)
{
    return find_ls();
}

static double cell_bpm(const unsigned char *p)
{
    double v;
    memcpy(&v, p + 8, 8);
    return v;
}

static long long cell_pos(const unsigned char *p)
{
    long long v;
    memcpy(&v, p, 8);
    return v;
}

/* The grid's sample rate, from its own cells: K beats span pos[K-1]-pos[0] samples and
 * sum(60/bpm) seconds. 0 = the grid disagrees with itself. */
static long grid_rate(const int64_t *pos, const double *bpm, int n)
{
    static const long rates[] = {44100, 48000, 88200, 96000};
    if (n < 2 || !pos || !bpm) return 0;
    double secs = 0.0;
    for (int i = 0; i < n - 1; i++) {
        if (!(bpm[i] >= 20.0 && bpm[i] <= 999.0)) return 0;
        if (pos[i] < -INT64_C(8294400000) || pos[i] > INT64_C(8294400000) ||
            pos[i + 1] <= pos[i] || pos[i + 1] > INT64_C(8294400000)) return 0;
        secs += 60.0 / bpm[i];
    }
    if (pos[n - 1] <= pos[0] || secs <= 0.0) return 0;
    double est = (double)(pos[n - 1] - pos[0]) / secs;
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++)
        if (est > rates[r] * 0.99 && est < rates[r] * 1.01) return rates[r];
    return 0;
}

static int read_grid(uint64_t ls, an_anlz_t *out, char *why, size_t why_n)
{
    uint64_t uso = q(ls + LS_PROV + 0x10);
    if (!uso || q(uso) != VT_USO_GRID) return -1;
    uint64_t c = q(uso + USO_CONTENT);
    if (!c || q(c) != VT_CONTENT_GRID) return -1;
    int32_t n = i32(c + GRID_NBEATS);
    uint64_t cells = q(c + GRID_CELLS);
    if (n < 2 || n > MAX_BEATS || !cells) {
        if (why && why_n) snprintf(why, why_n, "no usable beat grid in the loaded track");
        return -1;
    }
    int64_t *pos = malloc((size_t)n * sizeof *pos);
    double *bpm = malloc((size_t)n * sizeof *bpm);
    if (!pos || !bpm) {
        free(pos);
        free(bpm);
        return -1;
    }
    int ok = 1;
    int done = 0;
    while (done < n) {
        int take = n - done;
        if (take > 128) take = 128;
        unsigned char tmp[128 * 16];
        if (rd(cells + (uint64_t)done * 16, tmp, (size_t)take * 16) != 0) {
            ok = 0;
            break;
        }
        for (int i = 0; i < take; i++) {
            pos[done + i] = cell_pos(tmp + (size_t)i * 16);
            bpm[done + i] = cell_bpm(tmp + (size_t)i * 16);
        }
        done += take;
    }
    if (ok && (q(ls + LS_PROV + 0x10) != uso || q(uso + USO_CONTENT) != c ||
               i32(c + GRID_NBEATS) != n)) {
        free(pos);
        free(bpm);
        if (why && why_n) snprintf(why, why_n, "the track changed while its grid was read");
        return AN_LOADED_RETRY;
    }
    long rate = ok ? grid_rate(pos, bpm, n) : 0;
    if (!ok || !rate) {
        free(pos);
        free(bpm);
        if (why && why_n)
            snprintf(why, why_n, "the loaded beat grid was replaced or does not add up");
        return -1;
    }
    out->beats = calloc((size_t)n, sizeof *out->beats);
    if (!out->beats) {
        free(pos);
        free(bpm);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        int64_t p = pos[i];
        double ms = (double)p * 1000.0 / (double)rate;
        if (ms < 0) ms = 0;
        if (ms > 86400000.0) ms = 86400000.0; /* 24 h: a bad cell, not a track */
        out->beats[i].time_ms = (uint32_t)(ms + 0.5);
        double b = bpm[i];
        if (b < 20.0 || b > 400.0) b = bpm[0] >= 20.0 && bpm[0] <= 400.0 ? bpm[0] : 0.0;
        out->beats[i].tempo = (uint16_t)(b * 100.0 + 0.5);
        out->beats[i].beat_in_bar = (uint8_t)(i % 4 + 1);
    }
    out->nbeats = n;
    out->has_grid = 1;
    out->bpm = bpm[0] >= 20.0 && bpm[0] <= 400.0 ? bpm[0] : 0.0;
    free(pos);
    free(bpm);
    return 0;
}

static int read_phrases(uint64_t ls, an_anlz_t *out, char *why, size_t why_n)
{
    uint64_t tsd = ls + LS_TSD;
    if (q(tsd) != VT_TSD) return -1;
    uint64_t uso = q(tsd + 0x60);
    if (!uso) {
        if (why && why_n) snprintf(why, why_n, "the loaded track has no phrase analysis");
        return 1; /* A genuinely absent phrase map, rather than a failed copy. */
    }
    if (q(uso) != VT_USO_PHRASE) return -1;
    uint64_t c = q(uso + USO_CONTENT);
    if (!c) {
        if (why && why_n) snprintf(why, why_n, "the loaded track has no phrase analysis");
        return 1;
    }
    if (q(c) != VT_CONTENT_PHRASE) return -1;
    int32_t n = i32(c + PH_COUNT);
    int32_t n2 = i32(c + PH_COUNT2);
    int32_t genre = i32(c + PH_GENRE);
    uint64_t secs = q(c + PH_SECTIONS);
    if (n == 0) return 1;
    if (n < 0 || n > MAX_PHRASES || !secs || (n2 > 0 && n2 < n)) {
        if (why && why_n) snprintf(why, why_n, "the loaded track has no phrase analysis");
        return -1;
    }
    unsigned char *b = malloc((size_t)n * 0x14);
    if (!b) return -1;
    if (rd(secs, b, (size_t)n * 0x14) != 0) {
        free(b);
        if (why && why_n) snprintf(why, why_n, "the loaded phrase map could not be read");
        return -1;
    }
    /* the share and its content must not have been swapped while copying */
    if (q(tsd + 0x60) != uso || q(uso + USO_CONTENT) != c || i32(c + PH_COUNT) != n) {
        free(b);
        if (why && why_n) snprintf(why, why_n, "the track changed while its analysis was read");
        return -1;
    }
    out->phrases = calloc((size_t)n, sizeof *out->phrases);
    if (!out->phrases) {
        free(b);
        return -1;
    }
    int np = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char *s = b + (size_t)i * 0x14;
        int32_t kind, start, end;
        memcpy(&kind, s + 0, 4);
        memcpy(&start, s + 4, 4);
        memcpy(&end, s + 8, 4);
        if (kind < 0 || kind > 32 || start < 1 || end <= start || start > 65535 || end > 65535)
            continue;
        out->phrases[np].start_beat = (uint16_t)start;
        out->phrases[np].end_beat = (uint16_t)end;
        out->phrases[np].kind = (uint16_t)kind;
        np++;
    }
    free(b);
    if (np == 0) {
        free(out->phrases);
        out->phrases = NULL;
        if (why && why_n) snprintf(why, why_n, "the loaded phrase map is empty");
        return -1;
    }
    out->nphrases = np;
    out->has_phrases = 1;
    out->end_beat = out->phrases[np - 1].end_beat;
    out->mood = (genre >= 0 && genre <= 2) ? (uint16_t)(genre + 1) : 0;
    return 0;
}

/* Validate the grid and phrase providers as one snapshot. Checking each copy separately
 * accepts old-grid/new-phrases when the load changes between the two copies. */
typedef struct {
    uint64_t grid_share, grid, cells, phrase_share, phrase, sections, phrase_grid;
    int32_t nbeats, nphrases, nphrases2, genre;
} loaded_stamp_t;

static void loaded_stamp(uint64_t ls, loaded_stamp_t *s)
{
    memset(s, 0, sizeof *s);
    s->grid_share = q(ls + LS_PROV + 0x10);
    if (s->grid_share) s->grid = q(s->grid_share + USO_CONTENT);
    if (s->grid) {
        s->cells = q(s->grid + GRID_CELLS);
        s->nbeats = i32(s->grid + GRID_NBEATS);
    }
    s->phrase_grid = q(ls + LS_TSD + 0x68);
    s->phrase_share = q(ls + LS_TSD + 0x60);
    if (s->phrase_share) s->phrase = q(s->phrase_share + USO_CONTENT);
    if (s->phrase) {
        s->sections = q(s->phrase + PH_SECTIONS);
        s->nphrases = i32(s->phrase + PH_COUNT);
        s->nphrases2 = i32(s->phrase + PH_COUNT2);
        s->genre = i32(s->phrase + PH_GENRE);
    }
}

int an_loaded_read(int deck, an_anlz_t *out, char *why, size_t why_n)
{
    memset(out, 0, sizeof *out);
    if (why && why_n) why[0] = 0;
    if (deck < 0 || deck > 3) return -1;
    /* An early scan may have seen only part of the array. Do not permanently keep a
     * "ready" cache that is missing the requested deck. */
    if (g_scanned && !g_ls[deck].used) g_scanned = g_ready = 0;
    if (!find_ls() || !g_ls[deck].used) {
        if (why && why_n) snprintf(why, why_n, "the loaded state was not found");
        return AN_LOADED_RETRY;
    }
    uint64_t ls = g_ls[deck].obj;
    if (u32(ls + LS_MAGIC_OFF) != LS_MAGIC || q(ls) != VT_LOADSTATE || u32(ls + LS_DECK) != (unsigned)deck) {
        g_ls[deck].used = 0;
        g_ready = 0;
        g_scanned = 0;
        if (why && why_n) snprintf(why, why_n, "the loaded state changed");
        return AN_LOADED_RETRY;
    }
    loaded_stamp_t before, after;
    loaded_stamp(ls, &before);
    int grid = read_grid(ls, out, why, why_n);
    if (grid != 0) {
        an_anlz_free(out);
        return grid; /* Phrase beat numbers without a grid are not usable timestamps. */
    }
    int phr = read_phrases(ls, out, why, why_n);
    loaded_stamp(ls, &after);
    if (phr < 0 || memcmp(&before, &after, sizeof before) ||
        (phr == 0 && before.phrase_grid != before.grid_share) ||
        u32(ls + LS_MAGIC_OFF) != LS_MAGIC || q(ls) != VT_LOADSTATE ||
        u32(ls + LS_DECK) != (unsigned)deck) {
        an_anlz_free(out);
        if (why && why_n) snprintf(why, why_n, "the loaded grid and phrases changed while being read");
        return AN_LOADED_RETRY;
    }
    out->valid = 1;
    return 0;
}

/* Discover owned audio units once on the worker, with the same bounded read-only scan
 * policy as loaded analysis. Cached pointers are rechecked before and after every copy. */
static void find_loop_units(void)
{
    if (g_loop_scanned || g_fd < 0) return;
    struct timespec time;
    if(clock_gettime(CLOCK_MONOTONIC,&time)) return;
    uint64_t now=(uint64_t)time.tv_sec*1000+(uint64_t)time.tv_nsec/1000000;
    if(now<g_loop_scan_after) return;
    g_loop_scan_after=now+2000;
    g_loop_scanned = 1;
    char path[64], line[640];
    snprintf(path,sizeof path,"/proc/%d/maps",g_pid);
    FILE *f=fopen(path,"r");
    if (!f) { g_loop_scanned=0; return; }
    unsigned char *buf=malloc(SCAN_CHUNK);
    if (!buf) { fclose(f); g_loop_scanned=0; return; }
    struct loop_map { uint64_t lo,hi; } maps[2048];
    int nm=0;
    while(fgets(line,sizeof line,f) && nm<2048) {
        char perms[8];unsigned long long lo,hi;
        if(is_map_anon(line,perms,&lo,&hi) && hi>lo) {
            maps[nm].lo=lo;maps[nm++].hi=hi;
        }
    }
    /* Native units live in allocator arenas. Include all anonymous mappings (the deck
     * currently has >500), then search the large arenas first within the byte budget. */
    for(int i=1;i<nm;i++) {
        struct loop_map value=maps[i];int j=i-1;
        while(j>=0 && maps[j].hi-maps[j].lo<value.hi-value.lo) {
            maps[j+1]=maps[j];j--;
        }
        maps[j+1]=value;
    }
    uint64_t scanned=0;
    for(int m=0;m<nm && scanned<SCAN_MAX_BYTES;m++) {
        uint64_t hi=maps[m].hi;
        for(uint64_t at=maps[m].lo;at<hi && scanned<SCAN_MAX_BYTES;at+=SCAN_CHUNK) {
            size_t n=hi-at<SCAN_CHUNK?(size_t)(hi-at):SCAN_CHUNK;
            scanned+=n;
            if(rd(at,buf,n)) continue;
            for(size_t o=0;o+8<=n;o+=8) {
                uint64_t v;memcpy(&v,buf+o,8);
                if(v!=VT_LOOP_UNIT) continue;
                uint64_t unit=at+o,pub=q(unit+LOOP_PUBLISHER);
                if(q(pub)!=VT_LOOP_PUBLISHER || q(pub+8)!=VT_LOOP_PLAYER_BASE) continue;
                uint32_t deck=u32(pub+LOOP_DECK);
                if(deck<4) g_loop_units[deck]=unit;
            }
            if(g_loop_units[0] && g_loop_units[1] && g_loop_units[2] && g_loop_units[3]) goto done;
        }
    }
done:
    if(!g_loop_units[0] || !g_loop_units[1] || !g_loop_units[2] || !g_loop_units[3]) g_loop_scanned=0;
    free(buf);fclose(f);
}

static uint64_t loop_q(const unsigned char *p)
{
    uint64_t v;memcpy(&v,p,8);return v;
}
static uint32_t loop_u(const unsigned char *p)
{
    uint32_t v;memcpy(&v,p,4);return v;
}
static int loop_position_valid(const unsigned char *p)
{
    return p[0] && (p[4]&0x1f) && p[6] && loop_u(p+8) &&
           loop_q(p+16)!=INT64_MAX;
}
static int loop_point_ms(const unsigned char *point, uint32_t *out)
{
    if(loop_q(point)!=VT_LOOP_POINT || loop_q(point+0x10)!=VT_LOOP_POSITION ||
       !point[8] || !loop_position_valid(point+0x18)) return -1;
    uint32_t rate=loop_u(point+0x48);
    int64_t samples=(int64_t)loop_q(point+0x28), micros=(int64_t)loop_q(point+0x50);
    if(rate<8000 || rate>384000 || samples<0 || micros<0 || micros==INT64_MAX) return -1;
    /* The native setter caches floor(samples * 1e6 / Hertz). Verify both independent
     * representations rather than assuming the position's native units are ms. */
    double us=(double)samples*1000000.0/rate;
    if(!isfinite(us) || us>UINT32_MAX*1000.0 || fabs(us-micros)>2.0) return -1;
    *out=(uint32_t)llround(us/1000.0);
    return 0;
}
int an_loaded_loop_read(int deck, an_loaded_loop_t *out)
{
    if(!out || deck<0 || deck>3 || g_fd<0) return -1;
    memset(out,0,sizeof *out);
    find_loop_units();
    uint64_t unit=g_loop_units[deck], pub=q(unit+LOOP_PUBLISHER);
    if(!unit || q(unit)!=VT_LOOP_UNIT || q(pub)!=VT_LOOP_PUBLISHER ||
       q(pub+8)!=VT_LOOP_PLAYER_BASE || u32(pub+LOOP_DECK)!=(unsigned)deck) {
        g_loop_units[deck]=0;g_loop_scanned=0;return -1;
    }
    unsigned char before[0xd0], after[sizeof before];
    uint64_t frame=q(pub+0x68);
    if(rd(pub+LOOP_WORKING,before,sizeof before) || loop_q(before)!=VT_LOOP_PROPERTY) return -1;
    int engaged=loop_position_valid(before+0x30);
    an_loaded_loop_t value={.available=1,.engaged=engaged};
    if(engaged) {
        if(loop_point_ms(before+0x18,&value.in_ms) || loop_point_ms(before+0x70,&value.out_ms) ||
           value.out_ms<=value.in_ms || value.out_ms-value.in_ms<125 ||
           value.out_ms-value.in_ms>120000 || !frame ||
           loop_q(before+0x50)!=frame || loop_q(before+0xa8)!=frame ||
           loop_u(before+0x60)!=loop_u(before+0xb8)) return -1;
    }
    if(rd(pub+LOOP_WORKING,after,sizeof after) || memcmp(before,after,sizeof before) ||
       q(unit)!=VT_LOOP_UNIT || q(unit+LOOP_PUBLISHER)!=pub || q(pub)!=VT_LOOP_PUBLISHER ||
       q(pub+8)!=VT_LOOP_PLAYER_BASE || u32(pub+LOOP_DECK)!=(unsigned)deck ||
       q(pub+0x68)!=frame) return AN_LOADED_RETRY;
    *out=value;
    return 0;
}
