/* SPDX-License-Identifier: GPL-3.0-only */
#include "cues.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../store.c"
static char directory[256];
static int usb = 1;
static const char *data_dir(const char *id)
{
    assert(!strcmp(id, CUES_ID));
    return usb ? directory : NULL;
}
static int thread_start(const char *id, const char *name, void *(*fn)(void *), void *arg)
{
    (void)id;
    (void)name;
    (void)fn;
    (void)arg;
    return 0;
}
static void run_worker(void)
{
    assert(__atomic_load_n(&box.phase, __ATOMIC_ACQUIRE) == 2);
    __atomic_store_n(&box.phase, 3, __ATOMIC_RELAXED);
    perform();
    __atomic_store_n(&box.phase, 4, __ATOMIC_RELEASE);
}
static cues_config config, loaded;
static char status[160];
int main(void)
{
    snprintf(directory, sizeof directory, "/tmp/cues-store-XXXXXX");
    assert(mkdtemp(directory));
    az_core_v1 core = {.size = sizeof core, .data_dir = data_dir, .thread_start = thread_start};
    assert(cues_store_init(&core, &config, status, sizeof status));
    assert(!config.group_count);
    config.group_count = 1;
    cues_group_default(&config.groups[0], &config);
    snprintf(config.groups[0].name, sizeof config.groups[0].name, "Saved café");
    assert(cues_store_request(CUES_STORE_SAVE, &config, 7) == 1);
    assert(cues_store_request(CUES_STORE_EXPORT, &config, 7) == 0);
    run_worker();
    int op;
    unsigned rev;
    assert(cues_store_take(&loaded, &op, &rev, status, sizeof status) == 1 &&
           op == CUES_STORE_SAVE && rev == 7);
    assert(strstr(status, "saved"));
    assert(load_config(directory, "cue-rules.json", &loaded, status, sizeof status));
    assert(!strcmp(loaded.groups[0].name, "Saved café"));
    assert(cues_store_request(CUES_STORE_EXPORT, &config, 8) == 1);
    run_worker();
    assert(cues_store_take(&loaded, &op, &rev, status, sizeof status) == 1);
    char from[512], to[512];
    snprintf(from, sizeof from, "%s/cue-rules-export.json", directory);
    snprintf(to, sizeof to, "%s/cue-rules-import.json", directory);
    assert(rename(from, to) == 0);
    assert(cues_store_request(CUES_STORE_IMPORT, &config, 9) == 1);
    run_worker();
    memset(&loaded, 0, sizeof loaded);
    assert(cues_store_take(&loaded, &op, &rev, status, sizeof status) == 1 &&
           op == CUES_STORE_IMPORT && rev == 9);
    assert(!strcmp(loaded.groups[0].name, "Saved café"));
    cues_config before = loaded;
    FILE *bad = fopen(to, "w");
    assert(bad);
    fputs("{malformed}", bad);
    fclose(bad);
    assert(!load_config(directory, "cue-rules-import.json", &loaded, status, sizeof status));
    assert(!memcmp(&loaded, &before, sizeof before));
    usb = 0;
    assert(cues_store_request(CUES_STORE_SAVE, &config, 10) == 1);
    run_worker();
    assert(cues_store_take(&loaded, &op, &rev, status, sizeof status) == -1);
    assert(strstr(status, "USB unavailable"));
    usb = 1;
    unlink(to);
    assert(symlink("/dev/null", to) == 0);
    assert(!load_config(directory, "cue-rules-import.json", &loaded, status, sizeof status));
    unlink(to);
    snprintf(from, sizeof from, "%s/cue-rules.json", directory);
    unlink(from);
    assert(rmdir(directory) == 0);
    puts("Store passed: worker handoff, atomic save/reload, import/export, failed import "
         "retention, USB removal and symlink refusal");
    return 0;
}
