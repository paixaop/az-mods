/* SPDX-License-Identifier: GPL-3.0-only */
#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MA_NO_THREADING
#define MA_NO_RUNTIME_LINKING
#include "allocator.h"
#define MA_MALLOC(size) ta_malloc(size)
#define MA_REALLOC(pointer, size) ta_realloc(pointer, size)
#define MA_FREE(pointer) ta_free(pointer)
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "PortableDSP.h"
#include "aac.h"
struct ta_decoder { ma_decoder decoder; ta_aac *aac; uint64_t prefix, position; };
struct ta_mpeg_frame { unsigned rate, mode, bytes; };
static int ta_mpeg_header(const unsigned char h[4], struct ta_mpeg_frame *f) {
    static const unsigned rates[] = {44100, 48000, 32000};
    static const unsigned kbps[] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    unsigned rate = (h[2] >> 2) & 3, bit = h[2] >> 4;
    if (h[0] != 255 || (h[1] & 0xfe) != 0xfa || rate > 2 || !kbps[bit]) return 0;
    f->rate = rates[rate]; f->mode = h[3] >> 6;
    f->bytes = 144000 * kbps[bit] / f->rate + ((h[2] >> 1) & 1);
    return 1;
}
/* The original reader uses the encoded frame clock, with gapless trimming off.
 * Its metadata frame contributes silence only when three physical frame headers
 * agree. Validate the complete stream before enabling this decoding contract. */
static int ta_mpeg_timeline(const char *path, uint64_t *frames, uint64_t *prefix) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    unsigned char head[10];
    uint64_t at = 0, count = 0;
    struct ta_mpeg_frame first = {0}, normal = {0};
    int metadata = 0, compatible = 1, ok = 1;
    if (fread(head, 1, 10, file) != 10) { fclose(file); return 0; }
    if (!memcmp(head, "ID3", 3)) {
        if ((head[6] | head[7] | head[8] | head[9]) & 0x80) { fclose(file); return 0; }
        at = 10 + ((uint64_t)head[6] << 21 | (uint64_t)head[7] << 14 | (unsigned)head[8] << 7 | head[9]);
        if (head[5] & 0x10) at += 10;
    }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 0; }
    long size = ftell(file);
    if (size < 0 || (uint64_t)size > UINT64_C(1024)*1024*1024) { fclose(file); return 0; }
    while (at < (uint64_t)size) {
        if (fseek(file, (long)at, SEEK_SET)) { ok = 0; break; }
        unsigned char h[4];
        if (fread(h, 1, 4, file) != 4) { ok = 0; break; }
        if ((uint64_t)size - at == 128 && !memcmp(h, "TAG", 3)) break;
        struct ta_mpeg_frame f;
        if (!ta_mpeg_header(h, &f) || f.bytes > (uint64_t)size - at ||
            (f.rate != 44100 && f.rate != 48000)) { ok = 0; break; }
        if (!count) {
            first = f;
            unsigned offset = 4 + ((h[1] & 1) ? 0 : 2) + (f.mode == 3 ? 17 : 32);
            unsigned char marker[4];
            if (offset + 4 <= f.bytes && !fseek(file, (long)(at + offset), SEEK_SET) &&
                fread(marker, 1, 4, file) == 4)
                metadata = !memcmp(marker, "Info", 4) || !memcmp(marker, "Xing", 4);
        }
        if (metadata && count < 3 && (f.mode != first.mode || f.rate != first.rate)) compatible = 0;
        if (count == (unsigned)metadata) normal = f;
        if (count >= (unsigned)metadata && (f.mode != normal.mode || f.rate != normal.rate)) { ok = 0; break; }
        count++; at += f.bytes;
        if (count > (uint64_t)f.rate * 1200 / 1152 + 1) { ok = 0; break; }
    }
    fclose(file);
    if (!ok || count <= (unsigned)metadata) return 0;
    *prefix = metadata && compatible && count >= 3 ? 1152 : 0;
    *frames = (count - (unsigned)metadata) * 1152;
    return 1;
}
ta_decoder *ta_decode_open(const char *path, unsigned *rate, uint64_t *frames) {
    ta_decoder *d = ta_calloc(1, sizeof *d);
    if (!d) return NULL;
    /* MP3 probes scan for sync words inside arbitrary bytes. Recognize AAC
     * containers first, before compressed AAC payload can resemble MP3. */
    if (ta_aac_candidate(path)) {
        d->aac = ta_aac_open(path, rate, frames);
        if (d->aac) return d;
        ta_free(d); return NULL;
    }
    ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 2, 0);
    if (ma_decoder_init_file(path, &config, &d->decoder) != MA_SUCCESS) {
        d->aac = ta_aac_open(path, rate, frames);
        if (d->aac) return d;
        ta_free(d); return NULL;
    }
    if (d->decoder.pBackendVTable == &g_ma_decoding_backend_vtable_mp3) {
        uint64_t raw = 0;
        if (!ta_mpeg_timeline(path, &raw, &d->prefix)) {
            ma_decoder_uninit(&d->decoder); ta_free(d); return NULL;
        }
        ma_mp3 *backend = (ma_mp3 *)d->decoder.pBackend;
        backend->dr.delayInPCMFrames = backend->dr.paddingInPCMFrames = 0;
        backend->dr.totalPCMFrameCount = raw;
        /* Any seek table built at init used the gapless clock. The backend still
         * owns/frees that allocation; disable its borrowed lookup in the raw reader. */
        backend->dr.pSeekPoints = NULL;
        backend->dr.seekPointCount = 0;
        if (ma_decoder_seek_to_pcm_frame(&d->decoder, 0) != MA_SUCCESS) {
            ma_decoder_uninit(&d->decoder); ta_free(d); return NULL;
        }
    }
    if (d->decoder.outputSampleRate != 44100 && d->decoder.outputSampleRate != 48000) {
        ma_decoder_uninit(&d->decoder);
        config.sampleRate = 44100;
        if (ma_decoder_init_file(path, &config, &d->decoder) != MA_SUCCESS) { ta_free(d); return NULL; }
    }
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&d->decoder, &length) != MA_SUCCESS || !length ||
        length > (ma_uint64)d->decoder.outputSampleRate * 1200 ||
        d->prefix > (ma_uint64)d->decoder.outputSampleRate * 1200 - length) {
        ma_decoder_uninit(&d->decoder); ta_free(d); return NULL;
    }
    *rate = d->decoder.outputSampleRate;
    *frames = length + d->prefix;
    return d;
}
int64_t ta_decode_read(ta_decoder *d, int16_t *stereo, uint64_t frames) {
    if (d->aac) return ta_aac_read(d->aac, stereo, frames);
    uint64_t leading = d->position < d->prefix ? d->prefix - d->position : 0;
    if (leading > frames) leading = frames;
    memset(stereo, 0, (size_t)leading * 2 * sizeof *stereo);
    if (frames <= leading) { d->position += leading; return (int64_t)leading; }
    ma_uint64 read = 0;
    ma_result rc = ma_decoder_read_pcm_frames(&d->decoder, stereo + leading * 2, frames - leading, &read);
    d->position += leading + read;
    return rc == MA_SUCCESS || rc == MA_AT_END ? (int64_t)(read + leading) : -1;
}
int ta_decode_seek(ta_decoder *d, uint64_t frame) {
    if (d->aac) return ta_aac_seek(d->aac, frame);
    uint64_t decoded = frame > d->prefix ? frame - d->prefix : 0;
    if (ma_decoder_seek_to_pcm_frame(&d->decoder, decoded) != MA_SUCCESS) return -1;
    d->position = frame;
    return 0;
}
void ta_decode_get_clock(ta_decoder *d, ta_decode_clock *clock) {
    memset(clock, 0, sizeof *clock);
    if (d->aac) ta_aac_timeline(d->aac, clock);
}
void ta_decode_close(ta_decoder *d) {
    if (d) {
        if (d->aac) ta_aac_close(d->aac); else ma_decoder_uninit(&d->decoder);
        ta_free(d);
    }
}
