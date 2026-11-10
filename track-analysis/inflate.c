/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "inflate.h"
#include "vendor/miniz/miniz_tinfl.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
struct unpack {
    int fd;
    uint32_t crc, table[256];
    size_t count;
};
static int unpack_write(const void *data, int len, void *context) {
    struct unpack *u = context;
    if (len < 0 || (size_t)len > 128u * 1024u * 1024u - u->count) return 0;
    const uint8_t *bytes = data;
    for (int at = 0; at < len;) {
        ssize_t n = write(u->fd, bytes + at, (size_t)(len - at));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        at += (int)n;
    }
    for (int i = 0; i < len; i++) u->crc = u->table[(u->crc ^ bytes[i]) & 255] ^ (u->crc >> 8);
    u->count += (size_t)len;
    return 1;
}
static uint32_t unpack_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int ta_unpack_worker(const char *path, int output_fd) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    struct stat st;
    int ok = !fstat(fileno(f), &st) && S_ISREG(st.st_mode) && st.st_size > 18 && st.st_size <= 12 * 1024 * 1024;
    size_t n = ok ? (size_t)st.st_size : 0;
    uint8_t *bytes = ok ? malloc(n) : NULL;
    if (!bytes || fread(bytes, 1, n, f) != n) ok = 0;
    fclose(f);
    if (ok && (memcmp(bytes, "\x1f\x8b\x08\x00", 4) || !unpack_u32(bytes + n - 4))) ok = 0;
    if (ok) {
        struct unpack u = {.fd = output_fd, .crc = UINT32_MAX};
        for (unsigned i = 0; i < 256; i++) {
            uint32_t v = i;
            for (unsigned bit = 0; bit < 8; bit++) v = (v >> 1) ^ ((v & 1) ? UINT32_C(0xedb88320) : 0);
            u.table[i] = v;
        }
        size_t consumed = n - 18;
        ok = tinfl_decompress_mem_to_callback(bytes + 10, &consumed, unpack_write, &u, 0) &&
             consumed == n - 18 && u.count == unpack_u32(bytes + n - 4) &&
             (u.crc ^ UINT32_MAX) == unpack_u32(bytes + n - 8);
    }
    free(bytes);
    return ok;
}
