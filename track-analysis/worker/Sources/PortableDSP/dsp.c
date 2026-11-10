/* SPDX-License-Identifier: GPL-3.0-only */
#include "PortableDSP.h"
#include <math.h>
#include <stdlib.h>
#include "hmm-kernel.h"
const double *ta_hmm_kernel(size_t *width) {
    *width = 54;
    return &ta_native_hmm_kernel[0][0];
}
#define DEFINE_FFT(NAME, TYPE, COS, SIN)                                    \
void NAME(TYPE *r, TYPE *im, size_t n) {                                   \
    for (size_t i = 1, j = 0; i < n; i++) {                               \
        size_t bit = n >> 1;                                              \
        for (; j & bit; bit >>= 1) j ^= bit;                              \
        j ^= bit;                                                        \
        if (i < j) {                                                     \
            TYPE t = r[i]; r[i] = r[j]; r[j] = t;                         \
            t = im[i]; im[i] = im[j]; im[j] = t;                          \
        }                                                                \
    }                                                                    \
    for (size_t len = 2; len <= n; len <<= 1) {                           \
        double angle = -6.2831853071795864769 / (double)len;              \
        double cr = cos(angle), ci = sin(angle);                          \
        for (size_t start = 0; start < n; start += len) {                 \
            double wr = 1, wi = 0;                                       \
            for (size_t j = 0; j < len / 2; j++) {                       \
                size_t a = start + j, b = a + len / 2;                   \
                TYPE vr = r[b] * wr - im[b] * wi;                        \
                TYPE vi = r[b] * wi + im[b] * wr;                        \
                r[b] = r[a] - vr; im[b] = im[a] - vi;                     \
                r[a] += vr; im[a] += vi;                                 \
                double next = wr * cr - wi * ci;                        \
                wi = wr * ci + wi * cr; wr = next;                       \
            }                                                            \
        }                                                                \
    }                                                                    \
}
DEFINE_FFT(ta_fft_f, float, cosf, sinf)
DEFINE_FFT(ta_fft_d, double, cos, sin)

void ta_gemv(int transpose, int rows, int columns, const float *matrix,
             int stride, const float *input, float *output) {
    int outer = transpose ? columns : rows, inner = transpose ? rows : columns;
    for (int i = 0; i < outer; i++) {
        float sum = 0;
        for (int k = 0; k < inner; k++)
            sum += matrix[transpose ? k * stride + i : i * stride + k] * input[k];
        output[i] = sum;
    }
}
void ta_gemm(int transpose_b, int rows, int columns, int inner,
             const float *a, int a_stride, const float *b, int b_stride,
             float *output, int output_stride) {
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < columns; j++) {
            float sum = 0;
            for (int k = 0; k < inner; k++)
                sum += a[i * a_stride + k] * b[transpose_b ? j * b_stride + k : k * b_stride + j];
            output[i * output_stride + j] = sum;
        }
}
