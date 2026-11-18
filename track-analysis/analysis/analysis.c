/* SPDX-License-Identifier: GPL-3.0-only */
#include "azmod-timestamps.h"
#include "phrase.h"
struct ta_result {
    az_analysis_result_v1 view;
    az_analysis_phrase_v1 *phrases;
    az_vocal_frame_v1 *vocals;
};
const az_analysis_result_v1 *ta_result_view(const ta_result *result) {
    return result ? &result->view : NULL;
}
void ta_result_free(ta_result *result) {
    if (!result)
        return;
    ta_free(result->phrases);
    ta_free(result->vocals);
    ta_free(result);
}
typedef struct {
    ta_result *result;
    unsigned rate;
    uint64_t frames;
} analysis_stages;
static void run_stage(ta_context *c, void *data, size_t stage, unsigned worker) {
    (void)worker;
    analysis_stages *job = data;
    const ta_request *request = c->request;
    ta_result *result = job->result;
    unsigned rate = job->rate;
    uint64_t frames = job->frames;
    if (stage == 0) {
        ta_profile_begin(c);
        if (!request->beats || !request->beat_count || request->beat_count > 32768)
            ta_fail(c, TA_FAILED, "missing native beat grid");
        for (uint32_t i = 0; i < request->beat_count; i++) {
            const az_beat *b = &request->beats[i];
            if (!isfinite(b->time_s) || b->time_s < 0 || !isfinite(b->bpm) || b->bpm <= 0 ||
                b->bpm > 655.35 || b->beat_in_bar < 1 || b->beat_in_bar > 4 || b->reserved)
                ta_fail(c, TA_FAILED, "invalid native beat grid");
            if (i) {
                int order = compare_timestamps(b->time_s, request->beats[i - 1].time_s, 1, rate, 1);
                if (order == AZ_TIME_INVALID || order == AZ_TIME_EQUAL || order == AZ_TIME_BEFORE)
                    ta_fail(c, TA_FAILED, "unordered native beat grid");
            }
        }
        const az_beat *first = request->beats;
        double bpm = isfinite(request->bpm) && request->bpm > 0 ? request->bpm : first->bpm;
        if (bpm > 655.35)
            ta_fail(c, TA_FAILED, "invalid native tempo");
        Timing timing = reference_timing(c, bpm, first->time_s, first->beat_in_bar, first->bpm);
        Samples samples = phrase_decode(c);
        if ((unsigned)samples.rate != rate || (uint64_t)samples.frames + 1 != frames)
            ta_fail(c, TA_FAILED, "source clock changed");
        ta_profile_mark(c, "decode_bands_grid");
        PhraseExport phrases = phrase_classify(c, samples, timing);
        if (!phrases.map.count || phrases.map.count > 1024 || !phrases.map.end_beat ||
            phrases.map.end_beat > request->beat_count)
            ta_fail(c, TA_FAILED, "phrase result exceeds native grid");
        size_t size = phrases.map.count * sizeof *result->phrases;
        result->phrases = ta_malloc(size);
        if (!result->phrases)
            ta_fail(c, TA_LIMIT, "phrase result allocation failed");
        memcpy(result->phrases, phrases.map.entries, size);
        result->view.phrases = phrases.map;
        result->view.phrases.entries = result->phrases;
    } else {
        char model[1200];
        if (!request->assets_path || snprintf(model, sizeof model, "%s/pvdi-model.bin",
                                              request->assets_path) >= (int)sizeof model)
            ta_fail(c, TA_FAILED, "missing vocal model path");
        az_analysis_vocal_map_v1 vocals = ta_vocals(c, model);
        size_t size = vocals.count * sizeof *result->vocals;
        result->vocals = ta_malloc(size);
        if (!result->vocals)
            ta_fail(c, TA_LIMIT, "vocal result allocation failed");
        memcpy(result->vocals, vocals.frames, size);
        result->view.vocals = vocals;
        result->view.vocals.frames = result->vocals;
    }
    ta_cleanup(c);
}
int ta_analyze(const ta_request *request, ta_result **output, char *error, size_t error_size) {
    if (output)
        *output = NULL;
    if (error && error_size)
        error[0] = 0;
    if (!request || !output || !request->audio_path ||
        !(request->needed & (AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS)) ||
        (request->needed & ~(AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS))) {
        if (error && error_size)
            snprintf(error, error_size, "invalid analysis request");
        return TA_FAILED;
    }
    ta_execution execution;
    if (request->execution) execution = *request->execution;
    else if (ta_execution_environment(request->needed, &execution, error, error_size)) return TA_FAILED;
    const char *invalid = NULL;
    if (execution.mode != TA_STAGES_SEQUENTIAL && execution.mode != TA_STAGES_CONCURRENT)
        invalid = "invalidStageMode";
    else if (execution.phrase_threads < 1 || execution.phrase_threads > 3)
        invalid = "invalidThreadCount";
    else if (execution.vocal_threads < 1 || execution.vocal_threads > 3)
        invalid = "invalidVocalThreadCount";
    else if (execution.mode == TA_STAGES_CONCURRENT &&
             (request->needed & AZ_DATA_PHRASES) && (request->needed & AZ_ANALYSIS_VOCALS) &&
             execution.phrase_threads + execution.vocal_threads > 3)
        invalid = "concurrentThreadBudgetExceeded";
    if (invalid) {
        if (error && error_size) snprintf(error, error_size, "%s", invalid);
        return TA_FAILED;
    }
    ta_context *c = ta_calloc(1, sizeof *c);
    ta_result *result = ta_calloc(1, sizeof *result);
    if (!c || !result) {
        ta_free(c);
        ta_free(result);
        return TA_LIMIT;
    }
    c->request = request;
    c->execution = execution;
    if (setjmp(c->failure)) {
        int status = c->status;
        if (error && error_size)
            snprintf(error, error_size, "%s", c->error);
        ta_cleanup(c);
        ta_free(c);
        ta_result_free(result);
        return status;
    }
    ta_check(c);
    unsigned rate = 0;
    uint64_t frames = 0;
    c->decoder = ta_decode_open(request->audio_path, &rate, &frames);
    if (!c->decoder || (rate != 44100 && rate != 48000) || frames < 2 ||
        frames > (uint64_t)rate * 1200)
        ta_fail(c, TA_FAILED, "unsupported source clock or duration");
    ta_decode_close(c->decoder);
    c->decoder = NULL;
    result->view = (az_analysis_result_v1){.size = sizeof result->view,
                                           .version = AZ_DATA_VERSION,
                                           .source_rate = rate,
                                           .source_frames = frames};
    analysis_stages stages = {.result = result, .rate = rate, .frames = frames};
    if (execution.mode == TA_STAGES_CONCURRENT &&
        (request->needed & AZ_DATA_PHRASES) && (request->needed & AZ_ANALYSIS_VOCALS)) {
        ta_pair(c, run_stage, &stages);
    } else {
        if (request->needed & AZ_DATA_PHRASES) run_stage(c, &stages, 0, 0);
        if (request->needed & AZ_ANALYSIS_VOCALS) run_stage(c, &stages, 1, 0);
    }
    ta_check(c);
    ta_cleanup(c);
    ta_free(c);
    *output = result;
    return TA_OK;
}
