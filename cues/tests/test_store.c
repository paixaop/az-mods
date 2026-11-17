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
    assert(cues_store_request(&config, 7) == 1);
    assert(cues_store_request(&config, 7) == 0);
    run_worker();
    unsigned rev;
    assert(cues_store_take(&rev, status, sizeof status) == 1 && rev == 7);
    assert(strstr(status, "saved"));
    assert(load_config(directory, "cue-rules.json", &loaded, status, sizeof status));
    assert(!strcmp(loaded.groups[0].name, "Saved café"));
    /* Replacing the USB JSON directly takes effect at the next initialization. */
    snprintf(config.groups[0].name, sizeof config.groups[0].name, "USB replacement");
    char json[8192], path[512], replacement[512];
    int count = cues_config_json(&config, json, sizeof json);
    assert(count > 0);
    snprintf(path, sizeof path, "%s/cue-rules.json", directory);
    snprintf(replacement, sizeof replacement, "%s/replacement.json", directory);
    FILE *file = fopen(replacement, "w");
    assert(file);
    assert(fwrite(json, 1, (size_t)count, file) == (size_t)count);
    assert(fclose(file) == 0);
    assert(rename(replacement, path) == 0);
    memset(&loaded, 0, sizeof loaded);
    assert(cues_store_init(&core, &loaded, status, sizeof status));
    assert(!strcmp(loaded.groups[0].name, "USB replacement"));
    cues_config before = loaded;
    FILE *bad = fopen(path, "w");
    assert(bad);
    fputs("{malformed}", bad);
    fclose(bad);
    assert(!load_config(directory, "cue-rules.json", &loaded, status, sizeof status));
    assert(!memcmp(&loaded, &before, sizeof before));
    assert(cues_store_init(&core, &loaded, status, sizeof status));
    assert(!loaded.group_count && status[0]);
    usb = 0;
    assert(cues_store_request(&config, 10) == 1);
    run_worker();
    assert(cues_store_take(&rev, status, sizeof status) == -1);
    assert(strstr(status, "USB unavailable"));
    usb = 1;
    unlink(path);
    assert(symlink("/dev/null", path) == 0);
    assert(!load_config(directory, "cue-rules.json", &loaded, status, sizeof status));
    unlink(path);
    assert(rmdir(directory) == 0);
    puts("Store passed: worker handoff, atomic save/reload, direct JSON replacement, invalid JSON "
         "retention and startup fallback, USB removal and symlink refusal");
    return 0;
}
