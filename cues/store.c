/* SPDX-License-Identifier: GPL-3.0-only */
#include "store.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#define JSON_LIMIT (128u * 1024u)
static const az_core_v1 *SC;
/* Single producer/consumer mailbox. Only the thread owning the phase may access
 * payload; release/acquire transfers the whole bounded config without a UI lock. */
static struct {
    int phase, result, started;
    unsigned revision;
    cues_config config;
    char message[160];
} box;
static int message(char *out, size_t n, const char *s)
{
    if (n)
        snprintf(out, n, "%s", s);
    return 0;
}
static int config_file_valid(int fd)
{
    struct stat st;
    return !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 1 && st.st_size <= JSON_LIMIT;
}
static int read_config_bytes(int fd, char *json, size_t *length)
{
    *length = 0;
    while (*length < JSON_LIMIT + 1) {
        ssize_t got = read(fd, json + *length, JSON_LIMIT + 1 - *length);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return 0;
        if (!got) return 1;
        *length += (size_t)got;
    }
    return 0; /* Includes a file that grew beyond the validated stat size. */
}

static int load_config(const char *dir, const char *name, cues_config *out, char *error, size_t n)
{
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path)
        return message(error, n, "USB data path is too long");
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? message(error, n, "Configuration file not found")
                               : message(error, n, "Cannot open configuration file");
    if (!config_file_valid(fd)) {
        close(fd);
        return message(error, n, "Configuration must be a regular JSON file up to 128 KiB");
    }
    char *json = malloc(JSON_LIMIT + 1);
    if (!json) {
        close(fd);
        return message(error, n, "Out of memory");
    }
    size_t length;
    int okay = read_config_bytes(fd, json, &length);
    if (close(fd)) okay = 0;
    if (okay) okay = cues_config_parse(out, json, length, error, n);
    else message(error, n, "Cannot read bounded configuration");
    free(json);
    return okay;
}
static int write_config_bytes(int fd, const char *json, size_t count)
{
    size_t pos = 0;
    while (pos < (size_t)count) {
        ssize_t wrote = write(fd, json + pos, (size_t)count - pos);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            return 0;
        }
        pos += (size_t)wrote;
    }
    return 1;
}

static int write_config_file(const char *temp, const char *json, size_t count, char *error, size_t n)
{
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        message(error, n, "Cannot create configuration on USB");
        return -1;
    }
    int okay = write_config_bytes(fd, json, count);
    if (okay && fsync(fd)) okay = 0;
    if (close(fd)) okay = 0;
    return okay;
}

static int config_paths(const char *dir, const char *name, char path[1024], char temp[1024])
{
    return snprintf(path, 1024, "%s/%s", dir, name) < 1024 &&
           snprintf(temp, 1024, "%s/%s.tmp", dir, name) < 1024;
}
static int save_config(const char *dir, const char *name, const cues_config *config, char *error,
                       size_t n)
{
    char path[1024], temp[1024];
    if (!config_paths(dir, name, path, temp)) return message(error, n, "USB data path is too long");
    char *json = malloc(JSON_LIMIT + 1);
    if (!json)
        return message(error, n, "Out of memory");
    int count = cues_config_json(config, json, JSON_LIMIT + 1);
    if (count <= 0) {
        free(json);
        return message(error, n, "Configuration cannot be serialized");
    }
    int okay = write_config_file(temp, json, (size_t)count, error, n);
    free(json);
    if (okay < 0) return 0;
    if (okay && rename(temp, path))
        okay = 0;
    if (!okay) {
        unlink(temp);
        return message(error, n, "Cannot save configuration on USB");
    }
    if (n)
        error[0] = 0;
    return 1;
}
static void perform(void)
{
    const char *folder = SC->data_dir(CUES_ID);
    char dir[1024];
    if (!folder || !*folder) {
        box.result = 0;
        message(box.message, sizeof box.message, "USB unavailable; changes are not saved");
        return;
    }
    if (snprintf(dir, sizeof dir, "%s", folder) >= (int)sizeof dir) {
        box.result = 0;
        message(box.message, sizeof box.message, "USB data path is too long");
        return;
    }
    box.result = save_config(dir, "cue-rules.json", &box.config, box.message, sizeof box.message);
    if (box.result)
        snprintf(box.message, sizeof box.message, "Rules saved to USB");
}
static void store_step(void)
{
    int expected = 2;
    if (__atomic_compare_exchange_n(&box.phase, &expected, 3, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
        perform();
        __atomic_store_n(&box.phase, 4, __ATOMIC_RELEASE);
    }
}
static void *worker(void *unused)
{
    (void)unused;
    for (;;) {
        store_step();
        struct timespec pause = { 0, 100000000 };
        nanosleep(&pause, NULL);
    }
    return NULL;
}
int cues_store_init(const az_core_v1 *core, cues_config *config, char *status, size_t n)
{
    SC = core;
    cues_config_default(config);
    if (!AZ_HAS(core, data_dir) || !AZ_HAS(core, thread_start)) {
        message(status, n, "SDK storage unavailable; changes cannot be saved");
        return 0;
    }
    const char *dir = SC->data_dir(CUES_ID);
    if (dir) {
        char path[1024];
        snprintf(path, sizeof path, "%s/cue-rules.json", dir);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (!load_config(dir, "cue-rules.json", config, status, n))
                cues_config_default(config);
        } else
            message(status, n, "Create a group to begin");
    } else
        message(status, n, "USB unavailable; changes cannot be saved");
    if (box.started)
        return 1;
    if (core->thread_start(CUES_ID, "cues-settings", worker, NULL)) {
        message(status, n, "Cannot start settings storage worker");
        return 0;
    }
    box.started = 1;
    return 1;
}
int cues_store_request(const cues_config *config, unsigned revision)
{
    if (!box.started)
        return -1;
    int idle = 0;
    if (!__atomic_compare_exchange_n(&box.phase, &idle, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    box.config = *config;
    box.revision = revision;
    __atomic_store_n(&box.phase, 2, __ATOMIC_RELEASE);
    return 1;
}
int cues_store_take(unsigned *revision, char *status, size_t n)
{
    if (__atomic_load_n(&box.phase, __ATOMIC_ACQUIRE) != 4)
        return 0;
    *revision = box.revision;
    message(status, n, box.message);
    int result = box.result ? 1 : -1;
    __atomic_store_n(&box.phase, 0, __ATOMIC_RELEASE);
    return result;
}
