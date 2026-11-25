/* SPDX-License-Identifier: GPL-3.0-only */
#include "cues.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
static const char *grow_after_stat;
static int checked_stat(int fd, struct stat *st)
{
    int result = fstat(fd, st);
    if (!result && grow_after_stat) {
        int writer = open(grow_after_stat, O_WRONLY);
        assert(writer >= 0);
        assert(!ftruncate(writer, 128 * 1024 + 1));
        assert(!close(writer));
        grow_after_stat = NULL;
    }
    return result;
}
#define fstat checked_stat
#include "../store.c"
#undef fstat
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
    store_step();
    assert(__atomic_load_n(&box.phase, __ATOMIC_ACQUIRE) == 4);
    store_step();
    assert(__atomic_load_n(&box.phase, __ATOMIC_ACQUIRE) == 4);
}
static cues_config config, loaded;
static char status[160];
int main(void)
{
    snprintf(directory, sizeof directory, "/tmp/cues-store-XXXXXX");
    assert(mkdtemp(directory));
    az_core_v1 core = { .size = sizeof core, .data_dir = data_dir, .thread_start = thread_start };
    assert(cues_store_init(&core, &config, status, sizeof status));
    assert(!config.group_count);
    config.group_count = 1;
    cues_group *group = &config.groups[0];
    cues_group_default(group, &config);
    snprintf(group->name, sizeof group->name, "Saved café");
    group->rule_count = 1;
    cues_rule *rule = &group->rules[0];
    cues_rule_default(rule, group);
    rule->phrase_type = 4;
    rule->cue_type = AZ_CUE_MEMORY;
    rule->phrase_point = CUES_END;
    rule->offset = -16;
    assert(cues_store_request(&config, 7) == 1);
    assert(cues_store_request(&config, 7) == 0);
    run_worker();
    unsigned rev;
    assert(cues_store_take(&rev, status, sizeof status) == 1 && rev == 7);
    assert(strstr(status, "saved"));
    assert(load_config(directory, "cue-rules.json", &loaded, status, sizeof status));
    assert(!strcmp(loaded.groups[0].name, "Saved café"));
    assert(loaded.groups[0].rules[0].offset == -16);
    char json[8192], path[512], replacement[512];
    snprintf(path, sizeof path, "%s/cue-rules.json", directory);
    FILE *file = fopen(path, "r");
    assert(file);
    size_t saved = fread(json, 1, sizeof json - 1, file);
    assert(feof(file) && !fclose(file));
    json[saved] = 0;
    assert(strstr(json, "\"offset\":{\"value\":-16,\"unit\":\"bars\"}"));
    assert(strstr(json, "\"sentence\":\"Create a Memory Cue, at the end of all Chorus/Drop phrases"));
    /* Replacing the USB JSON directly takes effect at the next initialization. */
    snprintf(config.groups[0].name, sizeof config.groups[0].name, "USB replacement");
    int count = cues_config_json(&config, json, sizeof json);
    assert(count > 0);
    snprintf(replacement, sizeof replacement, "%s/replacement.json", directory);
    file = fopen(replacement, "w");
    assert(file);
    assert(fwrite(json, 1, (size_t)count, file) == (size_t)count);
    assert(fclose(file) == 0);
    assert(rename(replacement, path) == 0);
    memset(&loaded, 0, sizeof loaded);
    assert(cues_store_init(&core, &loaded, status, sizeof status));
    assert(!strcmp(loaded.groups[0].name, "USB replacement"));
    cues_config before = loaded;
    grow_after_stat = path;
    assert(!load_config(directory, "cue-rules.json", &loaded, status, sizeof status));
    assert(!grow_after_stat && strstr(status, "Cannot read bounded configuration"));
    assert(!memcmp(&loaded, &before, sizeof before));
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
    cues_config invalid = config;
    invalid.enabled = 2;
    assert(!save_config(directory, "cue-rules.json", &invalid, status, sizeof status));
    assert(strstr(status, "cannot be serialized"));
    char too_long[1100];
    memset(too_long, 'a', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = 0;
    assert(!save_config(too_long, "rules.json", &config, status, sizeof status));
    assert(strstr(status, "path is too long"));
    assert(!load_config(too_long, "rules.json", &loaded, status, sizeof status));
    assert(strstr(status, "path is too long"));
    snprintf(replacement, sizeof replacement, "%s/missing", directory);
    assert(!save_config(replacement, "rules.json", &config, status, sizeof status));
    assert(strstr(status, "Cannot create"));
    assert(mkdir(replacement, 0700) == 0);
    assert(!save_config(directory, "missing", &config, status, sizeof status));
    assert(strstr(status, "Cannot save"));
    assert(!load_config(directory, "missing", &loaded, status, sizeof status));
    assert(strstr(status, "regular JSON file"));
    assert(!rmdir(replacement));
    assert(!load_config(directory, "missing", &loaded, status, sizeof status));
    assert(strstr(status, "not found"));
    unlink(path);
    assert(symlink("/dev/null", path) == 0);
    assert(!load_config(directory, "cue-rules.json", &loaded, status, sizeof status));
    unlink(path);
    assert(rmdir(directory) == 0);
    puts(
        "Store passed: worker handoff, atomic save/reload, direct JSON replacement, invalid JSON "
        "retention and startup fallback, USB removal and symlink refusal");
    return 0;
}
