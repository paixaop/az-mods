/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TRACK_ANALYSIS_DSP_H
#define TRACK_ANALYSIS_DSP_H
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
void ta_fft_f(float *real, float *imaginary, size_t count);
void ta_fft_d(double *real, double *imaginary, size_t count);
const double *ta_hmm_kernel(size_t *width);
void ta_gemv(int transpose, int rows, int columns, const float *matrix,
             int stride, const float *input, float *output);
void ta_gemm(int transpose_b, int rows, int columns, int inner,
             const float *a, int a_stride, const float *b, int b_stride,
             float *output, int output_stride);
typedef struct ta_decoder ta_decoder;
enum { TA_DECODE_DEFAULT = 0, TA_DECODE_AAC_ENCODED = 1 };
typedef struct ta_decode_clock {
    uint32_t kind, has_edit;
    uint64_t decoder_leading_frames, edit_start_frames, presentation_frames;
} ta_decode_clock;
ta_decoder *ta_decode_open(const char *path, unsigned *rate, uint64_t *frames);
void ta_decode_get_clock(ta_decoder *decoder, ta_decode_clock *clock);
int64_t ta_decode_read(ta_decoder *decoder, int16_t *stereo, uint64_t frames);
int ta_decode_seek(ta_decoder *decoder, uint64_t frame);
void ta_decode_close(ta_decoder *decoder);
#endif
