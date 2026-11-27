/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t spawn(char *exe, const char *command, const char *directory, const char *value)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        char *args[] = {exe, (char *)command, (char *)directory, (char *)value, NULL, NULL, NULL};
        if (!strcmp(command, "download"))
            args[4] = (char *)directory;
        execv(exe, args);
        _exit(127);
    }
    return pid;
}

static int exit_status(pid_t pid)
{
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WEXITSTATUS(status);
}

static int run(char *exe, char *directory, const char *command, const char *value)
{
    return exit_status(spawn(exe, command, directory, value));
}

static void file_path(char *path, const char *dir, const char *name)
{
    assert(snprintf(path, PATH_MAX, "%s/%s", dir, name) < PATH_MAX);
}

static void result(const char *dir, char expected)
{
    char path[PATH_MAX];
    file_path(path, dir, "status");
    FILE *file = fopen(path, "r");
    assert(file && fgetc(file) == expected && fgetc(file) == EOF && fclose(file) == 0);
    assert(unlink(path) == 0);
}

static void cancellation(char *exe, const char *dir)
{
    char path[PATH_MAX];
    file_path(path, dir, "search.json");
    pid_t pid = spawn(exe, "search", dir, "block");
    const struct timespec delay = {.tv_nsec = 10000000};
    struct stat st;
    int started = 0;
    for (int i = 0; i < 500 && !started; ++i) {
        started = stat(path, &st) == 0 && st.st_size > 0;
        nanosleep(&delay, NULL);
    }
    assert(started && kill(pid, SIGTERM) == 0 && exit_status(pid) == 1);
    result(dir, '1');
    assert(access(path, F_OK) < 0);
}

static void search_cases(char *exe, char *dir)
{
    char path[PATH_MAX], json[512];
    assert(run(exe, dir, "search", "audio & literal") == 0);
    result(dir, '0');
    file_path(path, dir, "search.json");
    FILE *reply = fopen(path, "r");
    assert(reply && fgets(json, sizeof(json), reply) && fclose(reply) == 0);
    assert(strstr(json, "aqz-KE-bpKQ") && strstr(json, "Big Buck Bunny"));
    assert(strstr(json, "NOFORMAT000") && strstr(json, "VIDEOONLY00") && strstr(json, "MISSING0000"));
    assert(unlink(path) == 0);
    assert(run(exe, dir, "search", "empty") == 0);
    result(dir, '0');
    reply = fopen(path, "r");
    assert(reply && fgets(json, sizeof(json), reply) && !strcmp(json, "[]\n") && fclose(reply) == 0);
    assert(unlink(path) == 0);
    assert(run(exe, dir, "search", "fail") == 1);
    result(dir, '1');
    assert(access(path, F_OK) < 0);
    cancellation(exe, dir);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    char dir[PATH_MAX], path[PATH_MAX];
    file_path(dir, argv[2], "case.XXXXXX");
    assert(mkdtemp(dir));
    search_cases(argv[1], dir);
    assert(run(argv[1], dir, "unknown", "a") == 2);
    result(dir, '2');
    assert(run(argv[1], dir, "download", "../invalid") == 2);
    result(dir, '2');
    assert(run(argv[1], dir, "download", "NOFORMAT000") == 3);
    result(dir, '3');
    assert(run(argv[1], dir, "download", "MISSING0000") == 3);
    result(dir, '3');
    char remux[PATH_MAX];
    file_path(remux, dir, "remux-fail");
    assert(mkdir(remux, 0700) == 0);
    assert(run(argv[1], remux, "download", "aqz-KE-bpKQ") == 4);
    result(remux, '4');
    assert(rmdir(remux) == 0);
    assert(run(argv[1], dir, "download", "aqz-KE-bpKQ") == 0);
    result(dir, '0');
    assert(run(argv[1], dir, "download", "aqz-KE-bpKQ") == 5);
    result(dir, '5');
    file_path(path, dir, "aqz-KE-bpKQ.m4a");
    FILE *audio = fopen(path, "r");
    assert(audio && fgetc(audio) == 'a' && fclose(audio) == 0);
    assert(unlink(path) == 0 && rmdir(dir) == 0);
    puts("YouTube subprocess format, search, failure and publication paths passed");
    return 0;
}
