/* SPDX-License-Identifier: GPL-3.0-only
 *
 * resolve.c: locate the ANLZ pair for the track a deck loaded.
 */
#include "resolve.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_MOUNTS 8
#define MAX_FD_PATHS 8
#define PATH_N 512

typedef struct {
    char path[MAX_MOUNTS][PATH_N];
    int n;
} mounts_t;

static int path_is_dir(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_exists(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p ? p : "", '/');
    return s ? s + 1 : (p ? p : "");
}

static void strip_ext(char *s)
{
    char *dot = strrchr(s, '.');
    if (dot && dot != s) *dot = 0;
}

/* Compare with the path's extension removed. */
static int title_is_file(const char *title, const char *path)
{
    if (!title || !title[0] || !path || !path[0]) return 0;
    char a[PATH_N], b[PATH_N];
    snprintf(a, sizeof a, "%s", base_name(path));
    strip_ext(a);
    snprintf(b, sizeof b, "%s", title);
    return strcasecmp(a, b) == 0;
}

static int same_content_path(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0]) return 0;
    /* An open audio fd includes its mount prefix; PPTH uses the exported content path.
     * Preserve the directories: two artists can have the same filename. */
    const char *x = strstr(a, "/Contents/"), *y = strstr(b, "/Contents/");
    return strcasecmp(x ? x : a, y ? y : b) == 0;
}

static int has_ext(const char *p, const char *const *exts)
{
    const char *dot = strrchr(p, '.');
    if (!dot) return 0;
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    return 0;
}

static const char *const AUDIO_EXT[] = { ".mp3", ".m4a", ".aif", ".aiff", ".wav", ".flac",
                                         ".ogg", ".aac", NULL };

/* Rekordbox's analysis folder hash (validated in ANLZ-FINDINGS.md against a real USB:
 * 99.8 % of tracks; the remainder are re-analysed folders). The hash is over the UTF-16
 * code units of the content path. */
void an_anlz_folder_hash(const char *path, char *out, size_t out_n)
{
    uint32_t h = 0;
    const unsigned char *p = (const unsigned char *)path;
    while (p && *p) {
        unsigned cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            cp = ((unsigned)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((unsigned)(p[0] & 0x07) << 18) | ((unsigned)(p[1] & 0x3F) << 12) |
                 ((unsigned)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            cp = *p++;
        }
        if (cp < 0x10000) {
            h = (uint32_t)(cp * 0x93B6u + h * 0x34F5501Du);
        } else {
            unsigned v = cp - 0x10000;
            uint32_t hi = 0xD800u + (v >> 10), lo = 0xDC00u + (v & 0x3FFu);
            h = (uint32_t)(hi * 0x93B6u + h * 0x34F5501Du);
            h = (uint32_t)(lo * 0x93B6u + h * 0x34F5501Du);
        }
    }
    h %= 0x30D43u;
    unsigned sh = (unsigned)((h & 1u) | ((h >> 2) & 1u) << 1 | ((h >> 6) & 1u) << 2 |
                             ((h >> 7) & 1u) << 3 | ((h >> 9) & 1u) << 4 |
                             ((h >> 13) & 1u) << 5 | ((h >> 16) & 1u) << 6);
    snprintf(out, out_n, "P0%02X/%08X", sh, h);
}

static void add_mount(mounts_t *m, const char *path)
{
    if (!path || !path[0] || m->n >= MAX_MOUNTS) return;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    for (int i = 0; i < m->n; i++)
        if (strncmp(m->path[i], path, n) == 0 && m->path[i][n] == 0) return;
    snprintf(m->path[m->n], PATH_N, "%.*s", (int)n, path);
    m->n++;
}

static void add_mount_if_usb(mounts_t *m, const char *path)
{
    char probe[PATH_N + 16];
    if (!path_is_dir(path)) return;
    snprintf(probe, sizeof probe, "%s/PIONEER", path);
    if (path_is_dir(probe)) add_mount(m, path);
}

static void decode_mount(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' &&
            r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static void find_mounts(mounts_t *m, const az_core_v1 *C, const char *mod_id)
{
    (void)C;
    (void)mod_id;
    memset(m, 0, sizeof *m);

    FILE *f = fopen("/proc/mounts", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char dev[256], mp[PATH_N], type[64];
            if (sscanf(line, "%255s %511s %63s", dev, mp, type) != 3) continue;
            decode_mount(mp);
            add_mount_if_usb(m, mp);
        }
        fclose(f);
    }

    /* A few common automount parents, in case /proc/mounts is filtered. */
    static const char *const PARENTS[] = { "/media/usb", "/run/media/usb", "/media", NULL };
    for (int i = 0; PARENTS[i]; i++) {
        DIR *d = opendir(PARENTS[i]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char p[PATH_N];
            snprintf(p, sizeof p, "%s/%s", PARENTS[i], e->d_name);
            add_mount_if_usb(m, p);
            DIR *d2 = opendir(p);
            if (!d2) continue;
            struct dirent *e2;
            while ((e2 = readdir(d2)) != NULL) {
                if (e2->d_name[0] == '.') continue;
                char q[PATH_N * 3];
                snprintf(q, sizeof q, "%s/%s/%s", PARENTS[i], e->d_name, e2->d_name);
                add_mount_if_usb(m, q);
            }
            closedir(d2);
        }
        closedir(d);
    }
}

/* Try <mount>/PIONEER/USBANLZ/<hash>/ANLZ0000.{DAT,EXT} for a content path. */
static int try_content_path(const mounts_t *m, const char *content,
                            an_anlz_t *out, char *why, int why_n)
{
    char folder[64];
    an_anlz_folder_hash(content, folder, sizeof folder);
    for (int i = 0; i < m->n; i++) {
        char dir[PATH_N * 2];
        snprintf(dir, sizeof dir, "%s/PIONEER/USBANLZ/%s", m->path[i], folder);
        char dat[PATH_N * 2 + 32], ext[PATH_N * 2 + 32];
        snprintf(dat, sizeof dat, "%s/ANLZ0000.DAT", dir);
        snprintf(ext, sizeof ext, "%s/ANLZ0000.EXT", dir);
        if (!path_exists(dat) && !path_exists(ext)) {
            /* a re-analysed or multi-analysis folder: take the first pair there */
            DIR *d = opendir(dir);
            if (!d) continue;
            dat[0] = ext[0] = 0;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strncasecmp(e->d_name, "ANLZ", 4) != 0) continue;
                const char *dot = strrchr(e->d_name, '.');
                if (!dot) continue;
                char p[PATH_N * 2 + 32];
                snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
                if (strcasecmp(dot, ".DAT") == 0 && !dat[0]) snprintf(dat, sizeof dat, "%s", p);
                if (strcasecmp(dot, ".EXT") == 0 && !ext[0]) snprintf(ext, sizeof ext, "%s", p);
            }
            closedir(d);
        }
        if (!dat[0] && !ext[0]) continue;
        memset(out, 0, sizeof *out);
        if (an_anlz_load(dat[0] ? dat : NULL, ext[0] ? ext : NULL, out) != 0) {
            an_anlz_free(out);
            continue;
        }
        int ok = 1;
        if (out->audio_path[0] && content && content[0]) {
            if (!same_content_path(out->audio_path, content))
                ok = 0;
        }
        if (ok) return 0;
        an_anlz_free(out);
    }
    if (why && why_n) snprintf(why, (size_t)why_n, "no analysis file found for the track path");
    return -1;
}

typedef struct {
    uint32_t want;
    char want_title[128];
    char want_artist[128];
    char path[PATH_N];
    int found;
    int ambiguous;
} lib_ctx;

static int pl_cb(int pl_id, int parent, const char *name, int folder, void *user)
{
    (void)parent;
    (void)name;
    (void)folder;
    int *ids = user;
    if (ids[0] < 512) ids[++ids[0]] = pl_id;
    return 0;
}

static int trk_cb(const az_track *t, void *user)
{
    lib_ctx *c = user;
    if (!t || t->id != c->want || !t->path || !t->path[0]) return 0;
    int exact = 0;
    if (c->want_title[0]) {
        if (t->title && strcasecmp(t->title, c->want_title) == 0) exact = 1;
        else if (title_is_file(c->want_title, t->path)) exact = 1;
    }
    if (c->want_artist[0] && (!t->artist || strcasecmp(t->artist, c->want_artist))) exact = 0;
    if (exact) {
        if (c->found && strcmp(c->path, t->path)) c->ambiguous = 1;
        if (!c->found) snprintf(c->path, sizeof c->path, "%s", t->path);
        c->found = 1;
    }
    return 0;
}

static int library_path(const az_core_v1 *C, const char *mod_id, uint32_t content_id,
                        const char *title, const char *artist, char *path, size_t path_n)
{
    if (!C || !C->lib_playlists || !C->lib_tracks || !content_id || !title || !title[0]) return -1;
    char found[PATH_N] = "";
    for (int stick = 0; stick < 2; stick++) {
        int ids[513];
        memset(ids, 0, sizeof ids);
        if (C->lib_playlists(mod_id, stick, pl_cb, ids) < 0) continue;
        for (int i = 1; i <= ids[0]; i++) {
            lib_ctx ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.want = content_id;
            snprintf(ctx.want_title, sizeof ctx.want_title, "%s", title);
            snprintf(ctx.want_artist, sizeof ctx.want_artist, "%s", artist ? artist : "");
            C->lib_tracks(mod_id, stick, ids[i], trk_cb, &ctx);
            if (ctx.ambiguous) return -1;
            if (ctx.found) {
                if (found[0] && strcmp(found, ctx.path)) return -1;
                snprintf(found, sizeof found, "%s", ctx.path);
            }
        }
    }
    if (found[0]) {
        snprintf(path, path_n, "%s", found);
        return 0;
    }
    return -1;
}

static int scan_fds(char audio[MAX_FD_PATHS][PATH_N], int *naudio,
                    char anlz[MAX_FD_PATHS][PATH_N], int *nanlz)
{
    *naudio = *nanlz = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char link[PATH_N];
        snprintf(link, sizeof link, "/proc/self/fd/%s", e->d_name);
        char p[PATH_N];
        ssize_t n = readlink(link, p, sizeof p - 1);
        if (n <= 0) continue;
        p[n] = 0;
        char *deleted = strstr(p, " (deleted)");
        if (deleted) *deleted = 0;
        if (strstr(p, "/USBANLZ/") && has_ext(p, (const char *const[]){ ".DAT", ".EXT", ".2EX", NULL })) {
            if (*nanlz < MAX_FD_PATHS) snprintf(anlz[(*nanlz)++], PATH_N, "%s", p);
        } else if (strstr(p, "/Contents/") || has_ext(p, AUDIO_EXT)) {
            if (*naudio < MAX_FD_PATHS) snprintf(audio[(*naudio)++], PATH_N, "%s", p);
        }
    }
    closedir(d);
    return 0;
}

static int try_open_anlz(const char *path, const char *title, an_anlz_t *out)
{
    char dat[PATH_N * 2], ext[PATH_N * 2];
    snprintf(dat, sizeof dat, "%s", path);
    char *dot = strrchr(dat, '.');
    if (!dot) return -1;
    char stem[PATH_N * 2];
    snprintf(stem, sizeof stem, "%.*s", (int)(dot - dat), dat);
    snprintf(ext, sizeof ext, "%s.EXT", stem);
    snprintf(dat, sizeof dat, "%s.DAT", stem);
    memset(out, 0, sizeof *out);
    if (an_anlz_load(path_exists(dat) ? dat : NULL, path_exists(ext) ? ext : NULL, out) != 0) {
        an_anlz_free(out);
        return -1;
    }
    int ok = out->audio_path[0] && title_is_file(title, out->audio_path);
    if (!ok) {
        an_anlz_free(out);
        return -1;
    }
    return 0;
}

int an_resolve_analysis(const az_core_v1 *C, const char *mod_id, uint32_t content_id,
                        const char *title, const char *artist, an_anlz_t *out,
                        char *why, int why_n)
{
    memset(out, 0, sizeof *out);
    if (why && why_n) why[0] = 0;

    mounts_t m;
    find_mounts(&m, C, mod_id);

    char audio[MAX_FD_PATHS][PATH_N], anlz[MAX_FD_PATHS][PATH_N];
    int naudio = 0, nanlz = 0;
    scan_fds(audio, &naudio, anlz, &nanlz);

    /* 1. an ANLZ file EP147 itself has open */
    for (int i = 0; i < nanlz; i++)
        if (try_open_anlz(anlz[i], title, out) == 0) return 0;

    /* 2. an open audio file whose name matches the deck's title */
    for (int i = 0; i < naudio; i++)
        if (title_is_file(title, audio[i]) && m.n > 0 &&
            try_content_path(&m, audio[i], out, why, why_n) == 0)
            return 0;

    /* 3. the library service gives the content path for the deck's content id */
    if (content_id) {
        char path[PATH_N];
        if (library_path(C, mod_id, content_id, title, artist, path, sizeof path) == 0 && m.n > 0 &&
            try_content_path(&m, path, out, why, why_n) == 0)
            return 0;
        if (why && why_n) snprintf(why, (size_t)why_n, "content id %u not found in the playlists", content_id);
    }

    if (why && why_n && !why[0]) {
        if (!m.n) snprintf(why, (size_t)why_n, "no USB stick with a PIONEER folder is mounted");
        else if (!naudio && !nanlz) snprintf(why, (size_t)why_n, "track path unknown (not open, not in a playlist)");
        else snprintf(why, (size_t)why_n, "no matching analysis file");
    }
    return -1;
}
