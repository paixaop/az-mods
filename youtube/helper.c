/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE

#include "model.h"
#include "files.h"
#include "usb-publish.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WATCH_URL "https://www.youtube.com/watch?v="
static _Atomic int cancelled;

struct download_job {
    char scratch[PATH_MAX];
    char final[PATH_MAX];
};

static int helper_path(char *path, size_t capacity, const char *helper)
{
    ssize_t length = readlink("/proc/self/exe", path, capacity - 1);

    if (length < 0 || (size_t) length >= capacity - 1)
        return -1;
    path[length] = '\0';
    char *name = strrchr(path, '/');
    size_t helper_size = strlen(helper) + 1;
    if (!name || (size_t) (name + 1 - path) + helper_size > capacity)
        return -1;
    memcpy(name + 1, helper, helper_size);
    return 0;
}

static int prepare_job(struct download_job *job, const char *directory, const char *id)
{
    int final_length = snprintf(job->final, sizeof(job->final), "%s/%s.m4a", directory, id);
    int temp_length = snprintf(job->scratch, sizeof(job->scratch), "%s/.ytcui-audio.XXXXXX", directory);
    if (final_length < 0 || (size_t) final_length >= sizeof(job->final) ||
        temp_length < 0 || (size_t) temp_length + sizeof("/normalized.m4a") >= sizeof(job->scratch)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    struct stat existing;
    if (lstat(job->final, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }
    return errno == ENOENT && mkdtemp(job->scratch) ? 0 : -1;
}

static void cleanup_job(const char *scratch)
{
    DIR *directory = opendir(scratch);
    if (!directory)
        return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] != '.')
            (void) unlinkat(dirfd(directory), entry->d_name, 0);
    }
    closedir(directory);
    (void) rmdir(scratch);
}

static void cancel(int signum)
{
    (void)signum;
    cancelled = 1;
}

static int bind_parent(pid_t parent)
{
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
        return 0;
    return getppid() == parent;
}

static int child_stdout(int output)
{
    return output < 0 ? STDOUT_FILENO : dup2(output, STDOUT_FILENO);
}

static void execute_child(char *const args[], int output)
{
    if (child_stdout(output) >= 0)
        execv(args[0], args);
    _exit(127);
}

static void run_child(pid_t parent, char *const args[], int output)
{
    if (bind_parent(parent))
        execute_child(args, output);
    _exit(126);
}

static pid_t wait_child(pid_t pid, int *status)
{
    pid_t waited;
    do {
        waited = waitpid(pid, status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited;
}

static int child_result(pid_t pid, unsigned time_limit)
{
    int status;
    const struct timespec delay = {.tv_nsec = 50000000};
    for (unsigned i = 0; i < time_limit && !cancelled; ++i) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited != 0)
            return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        nanosleep(&delay, NULL);
    }
    (void)kill(pid, SIGKILL);
    (void)wait_child(pid, &status);
    return -1;
}

static int run(char *const args[], int output, unsigned seconds)
{
    if (cancelled)
        return -1;
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0)
        run_child(parent, args, output);
    return pid < 0 ? -1 : child_result(pid, seconds * 20);
}

static int capture(char *const args[], const char *path, unsigned seconds)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    int status = run(args, fd, seconds);
    if (close(fd))
        return -1;
    return status;
}

static int formats(const char *id, const char *path, YtFormat *format)
{
    char exe[PATH_MAX];
    if (helper_path(exe, sizeof(exe), "libexec/ytcui-dl") < 0)
        return -1;
    char url[sizeof(WATCH_URL) + YT_ID_SIZE];
    snprintf(url, sizeof(url), "%s%s", WATCH_URL, id);
    char *const args[] = {exe, "--yt-dlp", "-4", "--dump-json", "-f", "bestaudio", url, NULL};
    int status = capture(args, path, 9);
    size_t length;
    char *json = status == 0 ? yt_read_metadata(path, &length) : NULL;
    if (json) {
        status = yt_pick_audio(json, length, id, format);
        free(json);
    } else {
        status = -1;
    }
    (void)unlink(path);
    return status;
}

static int fetch_audio(const char *id, char *directory, int itag)
{
    char path[PATH_MAX];
    if (helper_path(path, sizeof(path), "libexec/ytcui-dl") < 0)
        return -1;
    char url[sizeof(WATCH_URL) + YT_ID_SIZE], format[24];
    snprintf(url, sizeof(url), "%s%s", WATCH_URL, id);
    snprintf(format, sizeof(format), "%d", itag);
    char *const args[] = {
        path, "--yt-dlp", "-4", "-N", "1",
        "-f", format,
        "-P", directory, "-o", "source.m4a", url, NULL,
    };
    return run(args, -1, 95);
}

static int normalize_audio(struct download_job *job)
{
    char path[PATH_MAX], input[PATH_MAX], output[PATH_MAX];
    if (helper_path(path, sizeof(path), "libexec/ffmpeg") < 0 ||
        yt_join_path(input, sizeof(input), job->scratch, "source.m4a") < 0 ||
        yt_join_path(output, sizeof(output), job->scratch, "normalized.m4a") < 0)
        return YT_HELPER_REMUX;
    if (access(input, R_OK) != 0) {
        fputs("A compatible M4A audio stream is unavailable.\n", stderr);
        return YT_HELPER_TRANSFER;
    }

    char *const args[] = {
        path, "-nostdin", "-v", "error", "-xerror", "-n", "-f", "mov", "-i", input,
        "-map", "0:a:0", "-c:a", "copy", "-movflags", "+faststart", "-f", "ipod", output, NULL,
    };
    if (run(args, -1, 15) < 0 || cancelled)
        return YT_HELPER_REMUX;
    if (yt_publish_usb(output, job->final) < 0) {
        perror(job->final);
        return YT_HELPER_USB;
    }
    return 0;
}

static int download_audio(const char *id, struct download_job *job)
{
    char path[PATH_MAX];
    YtFormat format;
    if (yt_join_path(path, sizeof(path), job->scratch, "formats.json") || formats(id, path, &format))
        return YT_HELPER_TRANSFER;
    if (fetch_audio(id, job->scratch, format.itag))
        return YT_HELPER_TRANSFER;
    return normalize_audio(job);
}

static int download(const char *id, const char *directory)
{
    if (!yt_valid_id(id))
        return YT_HELPER_INVALID;
    struct download_job job;
    if (prepare_job(&job, directory, id) < 0) {
        perror(job.final);
        return YT_HELPER_USB;
    }
    int status = download_audio(id, &job);
    cleanup_job(job.scratch);
    return status;
}

static int capture_search(char *query, const char *path)
{
    char exe[PATH_MAX];
    if (helper_path(exe, sizeof(exe), "libexec/ytcui-dl") < 0)
        return -1;
    char limit[12];
    snprintf(limit, sizeof(limit), "%d", YT_RESULT_LIMIT);
    char *const args[] = {exe, "-4", "--no-cache", "--json", "--max-results", limit,
                          "--search", query, NULL};
    return capture(args, path, 12);
}

static int empty_search(const char *path)
{
    /* ytcui-dl exits 1 with an empty JSON array for a search with no hits. */
    size_t length;
    char *json = yt_read_metadata(path, &length);
    int empty = json && (strcmp(json, "[]\n") == 0 || strcmp(json, "[]") == 0);
    free(json);
    return empty;
}

static int search(char *query, const char *directory)
{
    char path[PATH_MAX];
    if (yt_join_path(path, sizeof(path), directory, "search.json") < 0)
        return YT_HELPER_FAILED;
    int status = capture_search(query, path);
    if (!cancelled && (status == 0 || empty_search(path)))
        return YT_HELPER_OK;
    (void)unlink(path);
    return YT_HELPER_FAILED;
}

static void publish_status(const char *directory, int status)
{
    char tmp[PATH_MAX], final[PATH_MAX];
    if (yt_join_path(tmp, sizeof(tmp), directory, "status.tmp") ||
        yt_join_path(final, sizeof(final), directory, "status"))
        return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    const char result = (char)('0' + status);
    ssize_t written = write(fd, &result, sizeof(result));
    if (!close(fd) && written == sizeof(result))
        (void)rename(tmp, final);
    (void)unlink(tmp);
}

static int configure(void)
{
    struct sigaction signals = {.sa_handler = cancel};
    sigemptyset(&signals.sa_mask);
    return sigaction(SIGTERM, &signals, NULL) ||
           setenv("SSL_CERT_FILE", "/etc/ssl/certs/ca-certificates.crt", 0);
}

int main(int argc, char **argv)
{
    if (argc < 4 || configure())
        return YT_HELPER_INVALID;
    int status = YT_HELPER_INVALID;
    if (argc == 4 && !strcmp(argv[1], "search"))
        status = search(argv[3], argv[2]);
    if (argc == 5 && !strcmp(argv[1], "download"))
        status = download(argv[3], argv[4]);
    publish_status(argv[2], status);
    return status;
}
