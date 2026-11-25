/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod.h"
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bump this directory version when the record format or energy algorithm changes. */
#define CACHE_DIRECTORY "track-analysis-v3"

typedef struct {
    uint32_t id, key, major;
    char title[192], artist[128], key_text[32], path[SP_CACHE_PATH_MAX];
    double bpm, duration;
} CacheKey;

enum { CHOICES_PER_KIND = 1 + SP_OCCURRENCE_COUNT,
       SCORES = 1 + AZ_PHRASE_BRIDGE * CHOICES_PER_KIND };

typedef struct {
    CacheKey key;
    double raw_energy[SCORES];
    uint8_t encoding[SCORES];
    uint32_t full_track;
} CacheRecord;

static unsigned score_index(SpEnergyChoice choice)
{
    if (choice.mode == SP_FULL)
        return 0;
    unsigned base = 1 + ((unsigned)choice.kind - 1) * CHOICES_PER_KIND;
    return choice.mode == SP_PHRASES ? base : base + 1 + (unsigned)choice.occurrence;
}

static const char *relative_path(const char *directory, const char *track)
{
    static const char suffix[] = "/MODS/community-mods/" SP_ID "/data";
    size_t length = strlen(directory);
    if (length <= sizeof suffix - 1)
        return NULL;
    size_t root = length - (sizeof suffix - 1);
    if (strcmp(directory + root, suffix) || strncmp(directory, track, root))
        return NULL;
    return track[root] == '/' ? track + root : NULL;
}

static bool track_key(SpRuntime *runtime, int index, const char *directory, CacheKey *key)
{
    const SpModel *model = &runtime->model;
    const SpTrack *track = &model->tracks[index];
    const char *relative = relative_path(directory, model->inputs[index].path);
    if (!relative)
        return false;
    memset(key, 0, sizeof *key);
    key->id = model->source.rows[index].track_id;
    key->key = (uint32_t)track->key;
    key->major = track->major;
    key->bpm = track->bpm;
    key->duration = track->duration;
    memcpy(key->title, track->title, sizeof key->title);
    memcpy(key->artist, track->artist, sizeof key->artist);
    memcpy(key->key_text, track->key_text, sizeof key->key_text);
    snprintf(key->path, sizeof key->path, "%s", relative);
    return true;
}

static uint64_t cache_hash(const CacheKey *key)
{
    const unsigned char *bytes = (const unsigned char *)key;
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < sizeof *key; ++i) {
        value ^= bytes[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static int open_directory(int parent, const char *name, bool create)
{
    if (create)
        mkdirat(parent, name, 0700);
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static const char *storage_path(SpRuntime *runtime)
{
    return AZ_HAS(runtime->core, data_dir) ? runtime->core->data_dir(SP_ID) : NULL;
}

static int cache_bucket(const char *path, uint64_t hash, bool create)
{
    int usb = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (usb < 0)
        return -1;
    int root = open_directory(usb, CACHE_DIRECTORY, create);
    close(usb);
    if (root < 0)
        return -1;
    char name[3];
    snprintf(name, sizeof name, "%02x", (unsigned)(hash >> 56));
    int bucket = open_directory(root, name, create);
    close(root);
    return bucket;
}

static void cache_name(char *name, size_t length, uint64_t hash)
{
    snprintf(name, length, "%016" PRIx64, hash);
}

static bool read_record(int bucket, const char *name, CacheRecord *record)
{
    int fd = openat(bucket, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat info;
    bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size == (off_t)sizeof *record && read(fd, record, sizeof *record) == sizeof *record;
    close(fd);
    return valid;
}

static bool valid_energy(double score)
{
    return isfinite(score) && score > 0 && score <= 1;
}

static bool find_cache(const char *directory, const CacheKey *key, CacheRecord *record)
{
    uint64_t hash = cache_hash(key);
    int bucket = cache_bucket(directory, hash, false);
    if (bucket < 0)
        return false;
    char name[17];
    cache_name(name, sizeof name, hash);
    bool found = read_record(bucket, name, record);
    close(bucket);
    return found && memcmp(&record->key, key, sizeof *key) == 0;
}

static bool cached_score(const CacheRecord *record, unsigned slot)
{
    unsigned encoding = record->encoding[slot];
    return encoding >= AZ_WAVE_PWV3_MONO && encoding <= AZ_WAVE_PWV7_BANDS &&
        valid_energy(record->raw_energy[slot]);
}

bool sp_cache_read(SpRuntime *runtime, int index)
{
    CacheKey key;
    CacheRecord record;
    const char *directory = storage_path(runtime);
    if (!directory || !track_key(runtime, index, directory, &key))
        return false;
    if (!find_cache(directory, &key, &record))
        return false;
    unsigned slot = score_index(runtime->energy_choice);
    if (!cached_score(&record, slot))
        return false;
    SpInput *input = &runtime->model.inputs[index];
    input->score = record.raw_energy[slot];
    input->encoding = record.encoding[slot];
    input->energy = record.full_track & (UINT32_C(1) << slot) ? SP_FULL : runtime->energy_choice.mode;
    return true;
}

static void save_record(int bucket, const char *name, const CacheRecord *record)
{
    const char *temporary = ".new";
    unlinkat(bucket, temporary, 0);
    int fd = openat(bucket, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    bool written = write(fd, record, sizeof *record) == sizeof *record;
    if (close(fd) == 0 && written)
        renameat(bucket, temporary, bucket, name);
    unlinkat(bucket, temporary, 0);
}

static void update_record(int bucket, const char *name, const CacheKey *key, bool refresh,
                          unsigned slot, const SpInput *input)
{
    CacheRecord record;
    if (refresh || !read_record(bucket, name, &record) ||
        memcmp(key, &record.key, sizeof *key) != 0) {
        memset(&record, 0, sizeof record);
        memcpy(&record.key, key, sizeof *key);
    }
    if (input->energy == SP_MISSING || !valid_energy(input->score)) {
        unlinkat(bucket, name, 0);
        return;
    }
    record.raw_energy[slot] = input->score;
    record.encoding[slot] = (uint8_t)input->encoding;
    record.full_track &= ~(UINT32_C(1) << slot);
    if (input->energy == SP_FULL)
        record.full_track |= UINT32_C(1) << slot;
    save_record(bucket, name, &record);
}

void sp_cache_write(SpRuntime *runtime, int index, bool refresh)
{
    const SpInput *input = &runtime->model.inputs[index];
    bool known = input->energy != SP_MISSING;
    if (!known && !refresh)
        return;
    CacheKey key;
    const char *directory = storage_path(runtime);
    if (!directory || !track_key(runtime, index, directory, &key))
        return;
    uint64_t hash = cache_hash(&key);
    int bucket = cache_bucket(directory, hash, known);
    if (bucket < 0)
        return;
    char name[17];
    cache_name(name, sizeof name, hash);
    update_record(bucket, name, &key, refresh, score_index(runtime->energy_choice), input);
    close(bucket);
}
