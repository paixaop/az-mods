/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define an_anlz_load fixture_anlz_load
#include "resolve.c"
#undef an_anlz_load

static const char *saved_audio_path = "";
static const char *library_title = "Different track";
static const char *library_artist = "Artist";
static int ambiguous_paths;
static int playlists(const char *id, int stick, int (*fn)(int,int,const char*,int,void*), void *u)
{ (void)id; (void)stick; fn(1, 0, "Test", 0, u); return 0; }
static int tracks(const char *id, int stick, int pl, int (*fn)(const az_track*,void*), void *u)
{
    (void)id; (void)pl;
    az_track t = {.id = 7, .title = library_title, .artist = library_artist,
                  .path = ambiguous_paths && stick ? "/Contents/Other/Test.mp3" : "/Contents/Test.mp3"};
    fn(&t, u);
    return 0;
}
int fixture_anlz_load(const char *dat, const char *ext, an_anlz_t *out)
{
    (void)dat; (void)ext;
    memset(out, 0, sizeof *out);
    out->valid = 1;
    snprintf(out->audio_path, sizeof out->audio_path, "%s", saved_audio_path);
    return 0;
}

int main(void)
{
    az_core_v1 c = {.lib_playlists = playlists, .lib_tracks = tracks};
    char path[512];
    assert(library_path(&c, "anchor", 7, "Expected track", "Artist", path, sizeof path) != 0);
    library_title = "Expected track";
    assert(library_path(&c, "anchor", 7, "Expected track", "Artist", path, sizeof path) == 0);
    library_artist = "Wrong artist";
    assert(library_path(&c, "anchor", 7, "Expected track", "Artist", path, sizeof path) != 0);
    library_artist = "Artist"; ambiguous_paths = 1;
    assert(library_path(&c, "anchor", 7, "Expected track", "Artist", path, sizeof path) != 0);
    assert(!same_content_path("/Contents/A/Test.mp3", "/Contents/B/Test.mp3"));
    assert(same_content_path("/media/usb/PIONEER/../Contents/A/Test.mp3", "/Contents/A/Test.mp3"));
    an_anlz_t out;
    saved_audio_path = "";
    assert(try_open_anlz("/tmp/ANLZ0000.DAT", "Test", &out) != 0);
    saved_audio_path = "/Contents/Wrong.mp3";
    assert(try_open_anlz("/tmp/ANLZ0000.DAT", "Test", &out) != 0);
    saved_audio_path = "/Contents/Test.mp3";
    assert(try_open_anlz("/tmp/ANLZ0000.DAT", "Test", &out) == 0);
    an_anlz_free(&out);
    puts("resolver passed: title/artist matching, ambiguity rejection, full content paths and identified ANLZ only");
    return 0;
}
