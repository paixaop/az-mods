/* SPDX-License-Identifier: GPL-3.0-only
 * Synthetic loaded-state memory: no firmware, /proc or physical deck needed. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static ssize_t fixture_pread(int fd, void *out, size_t n, off_t at);
#define pread fixture_pread
#include "loaded.c"
#undef pread

static unsigned char memory[0x10000];
static int swap_grid, swap_loop;
enum { LOOP_UNIT_TEST=0xa000, LOOP_PUB_TEST=0xc000 };
enum { LS = 0x1000, GRID_SHARE = 0x3000, GRID = 0x4000, CELLS = 0x5000,
       PH_SHARE = 0x6000, PH = 0x7000, SECTIONS = 0x8000, OTHER_GRID = 0x9000 };

static void w64(size_t a, uint64_t v) { memcpy(memory + a, &v, sizeof v); }
static void w32(size_t a, int32_t v) { memcpy(memory + a, &v, sizeof v); }
static ssize_t fixture_pread(int fd, void *out, size_t n, off_t at)
{
    (void)fd;
    if (at < 0 || (uint64_t)at > sizeof memory || n > sizeof memory - (size_t)at) return -1;
    memcpy(out, memory + at, n);
    if (swap_grid && at == SECTIONS) {
        /* The new grid arrives after the old grid was copied, while phrases are read. */
        w64(GRID_SHARE + USO_CONTENT, OTHER_GRID);
        swap_grid = 0;
    }
    if(swap_loop && at==LOOP_PUB_TEST+LOOP_WORKING) {
        memory[LOOP_PUB_TEST+LOOP_WORKING+0x28]^=1;
        swap_loop=0;
    }
    return (ssize_t)n;
}

static void fixture(void)
{
    memset(memory, 0, sizeof memory);
    g_fd = 99; g_scanned = g_ready = 1;
    memset(g_ls, 0, sizeof g_ls);
    g_ls[0].obj = LS; g_ls[0].used = 1;
    w64(LS, VT_LOADSTATE); w32(LS + LS_DECK, 0); w32(LS + LS_MAGIC_OFF, LS_MAGIC);
    w64(LS + LS_PROV + 0x10, GRID_SHARE);
    w64(GRID_SHARE, VT_USO_GRID); w64(GRID_SHARE + USO_CONTENT, GRID);
    w64(GRID, VT_CONTENT_GRID); w64(GRID + GRID_CELLS, CELLS); w32(GRID + GRID_NBEATS, 16);
    for (int i = 0; i < 16; i++) {
        int64_t pos = i * 24000;
        double bpm = 120.0;
        memcpy(memory + CELLS + i * 16, &pos, 8);
        memcpy(memory + CELLS + i * 16 + 8, &bpm, 8);
    }
    w64(LS + LS_TSD, VT_TSD); w64(LS + LS_TSD + 0x60, PH_SHARE);
    w64(LS + LS_TSD + 0x68, GRID_SHARE);
    w64(PH_SHARE, VT_USO_PHRASE); w64(PH_SHARE + USO_CONTENT, PH);
    w64(PH, VT_CONTENT_PHRASE); w32(PH + PH_COUNT, 1); w32(PH + PH_COUNT2, 1);
    w32(PH + PH_GENRE, 0); w64(PH + PH_SECTIONS, SECTIONS);
    w32(SECTIONS, 5); w32(SECTIONS + 4, 1); w32(SECTIONS + 8, 9);
    swap_grid = 0;
}

static void loop_point_fixture(size_t at, int64_t samples)
{
    w64(at,VT_LOOP_POINT);w64(at+8,1);w64(at+0x10,VT_LOOP_POSITION);
    w64(at+0x18,UINT64_C(0x0001010200000001));w32(at+0x20,4);w64(at+0x28,(uint64_t)samples);
    w64(at+0x38,0x1234000);w64(at+0x40,0x1234000);
    w32(at+0x48,44100);w64(at+0x50,(uint64_t)((double)samples*1000000/44100));
}
static void loop_fixture(void)
{
    memset(memory,0,sizeof memory);g_fd=99;g_loop_scanned=1;
    memset(g_loop_units,0,sizeof g_loop_units);g_loop_units[1]=LOOP_UNIT_TEST;
    w64(LOOP_UNIT_TEST,VT_LOOP_UNIT);w64(LOOP_UNIT_TEST+LOOP_PUBLISHER,LOOP_PUB_TEST);
    w64(LOOP_PUB_TEST,VT_LOOP_PUBLISHER);w64(LOOP_PUB_TEST+8,VT_LOOP_PLAYER_BASE);
    w32(LOOP_PUB_TEST+LOOP_DECK,1);w64(LOOP_PUB_TEST+0x68,0x1234000);
    size_t working=LOOP_PUB_TEST+LOOP_WORKING;w64(working,VT_LOOP_PROPERTY);
    loop_point_fixture(working+0x18,0);loop_point_fixture(working+0x70,672000);
    swap_loop=0;
}
static void test_prepared_loop(void)
{
    an_loaded_loop_t state;
    loop_fixture();assert(an_loaded_loop_read(1,&state)==0);
    assert(state.available && state.engaged && state.in_ms==0 && state.out_ms==15238);
    size_t work=LOOP_PUB_TEST+LOOP_WORKING;
    loop_point_fixture(work+0x70,336000);assert(an_loaded_loop_read(1,&state)==0 && state.out_ms==7619);
    w64(work+0x30,0);assert(an_loaded_loop_read(1,&state)==0 && state.available && !state.engaged);
    loop_fixture();w32(LOOP_PUB_TEST+LOOP_DECK,0);assert(an_loaded_loop_read(1,&state)!=0 && !state.available);
    loop_fixture();w64(work+0xa8,0x3333);assert(an_loaded_loop_read(1,&state)!=0);
    loop_fixture();w64(work+0xc0,123);assert(an_loaded_loop_read(1,&state)!=0); /* rate/cache mismatch */
    loop_fixture();w64(work+0x98,INT64_MAX);assert(an_loaded_loop_read(1,&state)!=0);
    loop_fixture();swap_loop=1;assert(an_loaded_loop_read(1,&state)==AN_LOADED_RETRY && !state.available);
    loop_fixture();w64(LOOP_UNIT_TEST+LOOP_PUBLISHER,0);assert(an_loaded_loop_read(1,&state)!=0);
}

int main(void)
{
    an_anlz_t out;
    char why[160];
    fixture();
    assert(an_loaded_read(0, &out, why, sizeof why) == 0);
    assert(out.has_grid && out.has_phrases && out.nbeats == 16 && out.nphrases == 1);
    assert(out.beats[1].time_ms == 500);
    an_anlz_free(&out);
    fixture(); swap_grid = 1;
    assert(an_loaded_read(0, &out, why, sizeof why) != 0);
    assert(!out.valid && !out.beats && !out.phrases);
    fixture(); w64(LS + LS_TSD + 0x68, GRID_SHARE + 0x100);
    assert(an_loaded_read(0, &out, why, sizeof why) != 0);
    fixture(); w32(GRID + GRID_NBEATS, 0);
    assert(an_loaded_read(0, &out, why, sizeof why) != 0);
    fixture(); w64(CELLS + 8 * 16, 0); /* Non-monotonic grid with the same valid total duration. */
    assert(an_loaded_read(0, &out, why, sizeof why) != 0);
    fixture(); w32(LS + LS_DECK, 1);
    assert(an_loaded_read(0, &out, why, sizeof why) != 0);
    fixture(); w64(LS + LS_TSD + 0x60, 0); /* A genuine grid-only track is allowed. */
    assert(an_loaded_read(0, &out, why, sizeof why) == 0 && out.has_grid && !out.has_phrases);
    an_anlz_free(&out);
    fixture();
    assert(an_loaded_read(1,&out,why,sizeof why)==AN_LOADED_RETRY);
    assert(!out.valid && !out.beats && !g_scanned && !g_ls[0].used); /* stale partial cache is discarded */
    test_prepared_loop();
    puts("loaded and prepared-loop readers passed: consistent grid/phrase snapshots, deck identity and monotonic beats");
    return 0;
}
