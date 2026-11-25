/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod.h"
#include "azmod-timestamps.h"
#include <math.h>
#include <stdlib.h>

enum { WAVE_RATE = 150, PHRASE_LIMIT = 1024, POLL_LIMIT = 350 };
typedef struct { uint32_t start, end, kind; } Phrase;
typedef struct { uint32_t first, last; } Window;
typedef struct { Window items[PHRASE_LIMIT]; size_t count; } PhraseWindows;
typedef struct {
    double sum, low;
    unsigned count, active, attacks, last_attack;
} WaveFeatures;

static bool retryable(int status)
{
    return status == AZ_PENDING || status == AZ_BUSY;
}

static void release_ticket(SpRuntime *runtime, uint64_t ticket)
{
    for (int i = 0; i < POLL_LIMIT; ++i) {
        if (runtime->library->track_release(SP_ID, ticket) != AZ_BUSY)
            return;
        sp_delay();
    }
}

static uint64_t request_ticket(SpRuntime *runtime, const az_track_ref *track,
                               const az_wave_query *query)
{
    for (int i = 0; i < POLL_LIMIT && !sp_cancelled(runtime); ++i) {
        uint64_t ticket = 0;
        int status = query ? runtime->library->waveform_request(SP_ID, track, query, &ticket) :
            runtime->library->track_request(SP_ID, track, AZ_DATA_GRID | AZ_DATA_PHRASES, &ticket);
        if (status == AZ_OK || status == AZ_PENDING)
            return ticket;
        if (status != AZ_BUSY)
            break;
        sp_delay();
    }
    return 0;
}

static uint32_t phrase_kind(const az_phrase *phrase)
{
    return phrase->category ? phrase->category : az_phrase_decode(phrase->raw_mood, phrase->raw_kind);
}

static bool joined(const Phrase *previous, const Phrase *next)
{
    return previous->end == next->start && next->kind && previous->kind == next->kind;
}

static size_t coalesce(const az_track_snapshot *view, Phrase *phrases)
{
    size_t count = 0;
    for (uint32_t i = 0; i < view->phrase_count; ++i) {
        const az_phrase *saved = &view->phrases[i];
        Phrase phrase = {saved->start_beat, saved->end_beat, phrase_kind(saved)};
        if (phrase.end <= phrase.start || phrase.end > view->beat_count)
            continue;
        if (count && phrase.start < phrases[count - 1].end)
            continue;
        if (count && joined(&phrases[count - 1], &phrase))
            phrases[count - 1].end = phrase.end;
        else
            phrases[count++] = phrase;
    }
    return count;
}

static Window wave_bounds(double start, double end)
{
    /* The window is quantized to the PWV7 column clock. */
    int order = compare_timestamps(start, end, 1, WAVE_RATE, 1);
    if (order == AZ_TIME_INVALID || order == AZ_TIME_EQUAL)
        return (Window){0};
    if (order != AZ_TIME_BEFORE || start < 0 || end > 10000000.0 / WAVE_RATE)
        return (Window){0};
    return (Window){(uint32_t)ceil(start * WAVE_RATE), (uint32_t)ceil(end * WAVE_RATE)};
}

static Window phrase_window(const az_track_snapshot *view, Phrase phrase, double duration)
{
    uint32_t end = phrase.end;
    double start_s = view->beats[phrase.start].time_s;
    double end_s = end < view->beat_count ? view->beats[end].time_s : duration;
    return wave_bounds(start_s, end_s);
}

static bool usable_map(const az_track_snapshot *view)
{
    return view && view->grid_info.availability == AZ_AVAIL_READY &&
        view->phrases_info.availability == AZ_AVAIL_READY &&
        view->beats && view->phrases && view->phrase_count <= PHRASE_LIMIT;
}

static void select_windows(const az_track_snapshot *view, double duration, uint32_t kind,
                           PhraseWindows *out)
{
    if (!usable_map(view))
        return;
    Phrase phrases[PHRASE_LIMIT];
    size_t count = coalesce(view, phrases);
    for (size_t i = 0; i < count; ++i)
        if (phrases[i].kind == kind)
            out->items[out->count++] = phrase_window(view, phrases[i], duration);
}

static void request_windows(SpRuntime *runtime, const az_track_ref *track, double duration,
                            PhraseWindows *windows)
{
    uint64_t ticket = request_ticket(runtime, track, NULL);
    if (!ticket)
        return;
    for (int i = 0; i < POLL_LIMIT && !sp_cancelled(runtime); ++i) {
        const az_track_snapshot *view = NULL;
        int status = runtime->library->track_acquire(SP_ID, ticket, &view);
        if (retryable(status)) {
            sp_delay();
            continue;
        }
        if (status == AZ_OK)
            select_windows(view, duration, runtime->energy_choice.kind, windows);
        break;
    }
    release_ticket(runtime, ticket);
}

static bool low_attack(const WaveFeatures *f, double low)
{
    return f->count && low - f->low >= 8 && (!f->attacks || f->count - f->last_attack >= 15);
}

static unsigned column_bytes(uint32_t encoding)
{
    switch (encoding) {
    case AZ_WAVE_PWV7_BANDS: return 3;
    case AZ_WAVE_PWV5_RGB16: return 2;
    default: return 1;
    }
}

static double column_height(const uint8_t *bytes, uint32_t encoding)
{
    unsigned height = encoding == AZ_WAVE_PWV5_RGB16
        ? (bytes[1] >> 2) & 31u : bytes[0] & 31u;
    return height * (100.0 / 31.0);
}

static void add_feature(WaveFeatures *f, double level, double attack)
{
    if (low_attack(f, attack)) {
        ++f->attacks;
        f->last_attack = f->count;
    }
    f->low = attack;
    f->active += level >= 24;
    f->sum += level;
    ++f->count;
}

static void add_columns(WaveFeatures *f, const az_wave_snapshot *view)
{
    unsigned stride = view->bytes_per_column;
    for (uint32_t i = 0; i < view->column_count; ++i) {
        const uint8_t *bytes = view->columns + i * stride;
        if (view->encoding == AZ_WAVE_PWV7_BANDS) {
            double level = 0.50 * bytes[0] + 0.32 * bytes[1] + 0.18 * bytes[2];
            add_feature(f, level, bytes[0]);
        } else {
            double height = column_height(bytes, view->encoding);
            add_feature(f, height, height);
        }
    }
}

static bool wave_encoding_valid(const az_wave_snapshot *wave, const az_wave_query *query)
{
    if (!wave || !wave->columns || wave->encoding != query->encoding)
        return false;
    int clock = compare_timestamps(wave->seconds_per_column * WAVE_RATE, 1.0, 1, WAVE_RATE, 1);
    if (clock == AZ_TIME_INVALID)
        return false;
    return wave->bytes_per_column == column_bytes(query->encoding) &&
        wave->first_column == query->first_column && clock == AZ_TIME_EQUAL;
}

static bool wave_range_valid(const az_wave_snapshot *wave, const az_wave_query *query,
                             uint32_t total)
{
    uint32_t count = wave->column_count;
    if (count == 0 || count > query->column_count || count * wave->bytes_per_column != wave->data_bytes)
        return false;
    uint32_t end = wave->first_column + count;
    return (!total || wave->total_columns == total) && end <= wave->total_columns &&
        (count == query->column_count || end == wave->total_columns);
}

static bool read_wave(SpRuntime *runtime, const az_track_ref *track, const az_wave_query *query,
                      WaveFeatures *features, uint32_t *total)
{
    uint64_t ticket = request_ticket(runtime, track, query);
    if (!ticket)
        return false;
    bool valid = false;
    for (int i = 0; i < POLL_LIMIT && !sp_cancelled(runtime); ++i) {
        const az_wave_snapshot *wave = NULL;
        int status = runtime->library->waveform_acquire(SP_ID, ticket, &wave);
        if (retryable(status)) {
            sp_delay();
            continue;
        }
        valid = status == AZ_OK && wave_encoding_valid(wave, query) &&
            wave_range_valid(wave, query, *total);
        if (valid) {
            add_columns(features, wave);
            *total = wave->total_columns;
        }
        break;
    }
    release_ticket(runtime, ticket);
    return valid;
}

static double wave_score(const WaveFeatures *features, double bpm)
{
    double count = features->count;
    if (!features->count || features->sum <= 0)
        return NAN;
    double beats = count / WAVE_RATE * (bpm > 0 ? bpm : 120.0) / 60;
    return 0.58 * sp_clamp(features->sum / count / 100, 0, 1) +
           0.27 * features->active / count +
           0.15 * sp_clamp(features->attacks / beats, 0, 1);
}

static double score_window(SpRuntime *runtime, const az_track_ref *track, Window window, double bpm,
                           uint32_t encoding)
{
    WaveFeatures features = {0};
    uint32_t total = 0;
    while (window.first + features.count < window.last) {
        uint32_t first = window.first + features.count;
        uint32_t count = window.last - first;
        az_wave_query query = { .size = sizeof query, .version = AZ_DATA_VERSION,
            .encoding = encoding, .first_column = first,
            .column_count = count < AZ_WAVE_WINDOW_MAX ? count : AZ_WAVE_WINDOW_MAX };
        if (!read_wave(runtime, track, &query, &features, &total))
            return NAN;
        if (window.first + features.count >= total)
            break;
    }
    return wave_score(&features, bpm);
}

static int compare_score(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static double median_windows(SpRuntime *runtime, const az_track_ref *track,
                             const PhraseWindows *windows, double bpm, uint32_t encoding)
{
    double scores[PHRASE_LIMIT];
    size_t count = 0;
    for (size_t i = 0; i < windows->count && !sp_cancelled(runtime); ++i) {
        double score = score_window(runtime, track, windows->items[i], bpm, encoding);
        if (!isfinite(score))
            return NAN;
        scores[count++] = score;
    }
    if (!count)
        return NAN;
    qsort(scores, count, sizeof *scores, compare_score);
    return (scores[count / 2] + scores[(count - 1) / 2]) / 2;
}

static size_t occurrence_index(enum SpOccurrence occurrence, size_t count)
{
    switch (occurrence) {
    case SP_MIDDLE: return (count - 1) / 2;
    case SP_LAST: return count - 1;
    default: return 0;
    }
}

static bool resolve_track(SpRuntime *runtime, int index, az_track_ref *track)
{
    const SpOrder *source = &runtime->model.source;
    int status = runtime->library->track_resolve(SP_ID, runtime->model.stick,
                                                source->rows[index].track_id, track);
    return status == AZ_OK && track->source_generation == source->identity.source_generation &&
        track->library_id == source->identity.library_id;
}

static const uint32_t detail_formats[] = {
    AZ_WAVE_PWV7_BANDS, AZ_WAVE_PWV5_RGB16, AZ_WAVE_PWV3_MONO
};

static bool store_score(SpInput *input, double score, enum SpEnergy mode, uint32_t encoding)
{
    if (!isfinite(score))
        return false;
    input->score = score;
    input->energy = mode;
    input->encoding = encoding;
    return true;
}

static void score_full(SpRuntime *runtime, const az_track_ref *track, double bpm, SpInput *input)
{
    for (size_t i = 0; i < sizeof detail_formats / sizeof *detail_formats; ++i) {
        if (sp_cancelled(runtime))
            return;
        uint32_t format = detail_formats[i];
        double score = score_window(runtime, track, (Window){0, UINT32_MAX}, bpm, format);
        if (store_score(input, score, SP_FULL, format))
            return;
    }
}

static bool score_phrases(SpRuntime *runtime, const az_track_ref *track, double bpm,
                          const PhraseWindows *windows, SpInput *input)
{
    SpEnergyChoice choice = runtime->energy_choice;
    size_t index = occurrence_index(choice.occurrence, windows->count);
    for (size_t i = 0; i < sizeof detail_formats / sizeof *detail_formats; ++i) {
        if (sp_cancelled(runtime))
            return false;
        uint32_t format = detail_formats[i];
        double score = choice.mode == SP_PHRASES
            ? median_windows(runtime, track, windows, bpm, format)
            : score_window(runtime, track, windows->items[index], bpm, format);
        if (store_score(input, score, choice.mode, format))
            return true;
    }
    return false;
}

static bool try_phrases(SpRuntime *runtime, const az_track_ref *track, const SpTrack *info,
                        SpInput *input)
{
    if (runtime->energy_choice.mode == SP_FULL)
        return false;
    PhraseWindows windows;
    windows.count = 0;
    request_windows(runtime, track, info->duration, &windows);
    return windows.count && score_phrases(runtime, track, info->bpm, &windows, input);
}

void sp_analyze_track(SpRuntime *runtime, int index)
{
    az_track_ref track = {.size = sizeof track, .version = AZ_DATA_VERSION};
    if (!resolve_track(runtime, index, &track))
        return;
    SpInput *input = &runtime->model.inputs[index];
    const SpTrack *info = &runtime->model.tracks[index];
    input->energy = SP_MISSING;
    input->encoding = 0;
    if (!try_phrases(runtime, &track, info, input))
        score_full(runtime, &track, info->bpm, input);
}
