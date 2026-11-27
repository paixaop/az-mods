/* SPDX-License-Identifier: GPL-3.0-only */
#include "files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int yt_join_path(char *path, size_t capacity, const char *directory, const char *name)
{
    int n = snprintf(path, capacity, "%s/%s", directory, name);
    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}

static FILE *open_file(const char *path, struct stat *st)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return NULL;
    FILE *file = NULL;
    if (!fstat(fd, st) && S_ISREG(st->st_mode))
        file = fdopen(fd, "r");
    if (!file)
        close(fd);
    return file;
}

static char *read_file(FILE *file, size_t size, size_t *length)
{
    char *contents = malloc(size + 1);
    if (!contents)
        return NULL;
    if (fread(contents, 1, size, file) != size || fgetc(file) != EOF) {
        free(contents);
        return NULL;
    }
    contents[size] = '\0';
    *length = size;
    return contents;
}

char *yt_read_metadata(const char *path, size_t *length)
{
    struct stat st;
    FILE *file = open_file(path, &st);
    if (!file)
        return NULL;
    char *contents = NULL;
    if (st.st_size >= 0 && st.st_size <= YT_METADATA_LIMIT)
        contents = read_file(file, (size_t)st.st_size, length);
    fclose(file);
    return contents;
}

static int track_path(char *path, size_t capacity, const char *dir, const char *id,
                      const char *suffix)
{
    if (!yt_valid_id(id))
        return -1;
    int n = snprintf(path, capacity, "%s/%s%s", dir, id, suffix);
    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}

int yt_audio_path(char *path, size_t capacity, const char *dir, const char *id)
{
    return track_path(path, capacity, dir, id, ".m4a");
}

int yt_audio_ready(const char *dir, const char *id)
{
    char path[PATH_MAX];
    struct stat st;
    return yt_audio_path(path, sizeof(path), dir, id) == 0 &&
           lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static int copy_audio(FILE *in, FILE *out, off_t remaining)
{
    unsigned char buffer[16384];
    while (remaining > 0) {
        size_t count = remaining < (off_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        if (fread(buffer, 1, count, in) != count || fwrite(buffer, 1, count, out) != count)
            return -1;
        remaining -= (off_t)count;
    }
    return fgetc(in) == EOF && !ferror(in) ? 0 : -1;
}

static int stage_audio(FILE *in, const char *path, off_t bytes)
{
    /* Keep an existing file if the caller supplied a nonempty directory. */
    FILE *out = fopen(path, "wbx");
    if (!out)
        return -1;
    int status = copy_audio(in, out, bytes);
    if (fclose(out) != 0)
        status = -1;
    if (status)
        (void)unlink(path);
    return status;
}

int yt_stage_cached_track(const char *directory, const char *id, const char *destination)
{
    char source[PATH_MAX];
    if (yt_audio_path(source, sizeof(source), directory, id))
        return -1;
    struct stat st;
    FILE *in = open_file(source, &st);
    if (!in)
        return 1;
    int status = st.st_size > 0 ? stage_audio(in, destination, st.st_size) : 1;
    fclose(in);
    return status;
}

static int read_line(FILE *file, char *out, size_t size)
{
    if (!fgets(out, (int)size, file))
        return -1;
    char *end = strchr(out, '\n');
    if (!end)
        return fgetc(file) == '\n' ? 0 : -1;
    *end = '\0';
    return 0;
}

static int load_track(FILE *file, YtTrack *track)
{
    char seconds[24];
    if (read_line(file, track->title, sizeof(track->title)) ||
        read_line(file, track->channel, sizeof(track->channel)) ||
        read_line(file, seconds, sizeof(seconds)))
        return -1;
    char *end;
    errno = 0;
    long value = strtol(seconds, &end, 10);
    if (!errno && !*end && value >= 0 && value <= INT_MAX)
        track->duration_s = (int)value;
    return 0;
}

static int filename_id(const char *name, char *id)
{
    if (strlen(name) != YT_ID_SIZE + 3 || strcmp(name + YT_ID_SIZE - 1, ".m4a"))
        return 0;
    memcpy(id, name, YT_ID_SIZE - 1);
    id[YT_ID_SIZE - 1] = '\0';
    return yt_valid_id(id);
}

static YtTrack catalog_track(const char *directory, const char *id)
{
    char path[PATH_MAX];
    YtTrack track = {0};
    memcpy(track.id, id, sizeof(track.id));
    if (!track_path(path, sizeof(path), directory, id, ".txt")) {
        struct stat st;
        FILE *file = open_file(path, &st);
        if (file) {
            (void)load_track(file, &track);
            fclose(file);
        }
    }
    if (!track.title[0])
        memcpy(track.title, id, sizeof(track.id));
    return track;
}

static int catalog_page(DIR *dir, const char *path, size_t offset, YtTrack *items,
                        size_t capacity, int *more)
{
    size_t skipped = 0, count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char id[YT_ID_SIZE];
        if (!filename_id(entry->d_name, id) || !yt_audio_ready(path, id))
            continue;
        if (skipped < offset) {
            ++skipped;
            continue;
        }
        if (count == capacity) {
            *more = 1;
            break;
        }
        items[count++] = catalog_track(path, id);
    }
    return (int)count;
}

int yt_list_tracks(const char *directory, size_t offset, YtTrack *items, size_t capacity, int *more)
{
    *more = 0;
    DIR *dir = opendir(directory);
    if (!dir)
        return errno == ENOENT ? 0 : -1;
    int count = catalog_page(dir, directory, offset, items, capacity, more);
    closedir(dir);
    return count;
}
