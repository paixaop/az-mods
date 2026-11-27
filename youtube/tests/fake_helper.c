/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *arg(char **argv, const char *option)
{
    for (int i = 1; argv[i]; ++i)
        if (!strcmp(argv[i], option))
            return argv[i + 1];
    return NULL;
}

static void write_audio(const char *path)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs("audio fixture", file) >= 0 && fclose(file) == 0);
}

int main(int argc, char **argv)
{
    if (strstr(argv[0], "/ffmpeg")) {
        assert(!strcmp(arg(argv, "-map"), "0:a:0"));
        assert(!strcmp(arg(argv, "-c:a"), "copy"));
        assert(access(arg(argv, "-i"), R_OK) == 0);
        if (strstr(arg(argv, "-i"), "/remux-fail/"))
            return 1;
        write_audio(argv[argc - 1]);
        return 0;
    }
    if (arg(argv, "--search")) {
        if (!strcmp(arg(argv, "--search"), "fail"))
            return 9;
        if (!strcmp(arg(argv, "--search"), "block")) {
            assert(puts("waiting") >= 0 && fflush(stdout) == 0);
            sleep(30);
            return 0;
        }
        if (!strcmp(arg(argv, "--search"), "empty")) {
            puts("[]");
            return 1;
        }
        assert(!strcmp(arg(argv, "--search"), "audio & literal"));
        puts("[{\"id\":\"NOFORMAT000\"},{\"id\":\"aqz-KE-bpKQ\",\"title\":\"Big Buck Bunny\"},"
             "{\"id\":\"VIDEOONLY00\"},{\"id\":\"MISSING0000\"}]");
        return 0;
    }
    const char *url = argv[argc - 1];
    const char *id = strchr(url, '=');
    assert(id);
    if (!strcmp(arg(argv, "-f"), "bestaudio")) {
        printf("{\"id\":\"%s\",\"formats\":[", id + 1);
        if (!strcmp(id + 1, "VIDEOONLY00"))
            fputs("{\"itag\":137,\"has_video\":true,\"vcodec\":\"avc1\"}", stdout);
        if (!strcmp(id + 1, "aqz-KE-bpKQ") || !strcmp(id + 1, "MISSING0000"))
            fputs("{\"itag\":140,\"has_audio\":true,\"vcodec\":\"none\",\"container\":\"mp4\","
                  "\"acodec\":\"mp4a.40.2\",\"audio_channels\":2,\"audio_sample_rate\":44100,"
                  "\"average_bitrate\":129481}", stdout);
        puts("]}");
        return 0;
    }
    assert(!strcmp(arg(argv, "-f"), "140"));
    if (!strcmp(id + 1, "MISSING0000"))
        return 0;
    if (!strcmp(id + 1, "NOFORMAT000"))
        return 1;
    char path[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/source.m4a", arg(argv, "-P")) < (int)sizeof(path));
    write_audio(path);
    return 0;
}
