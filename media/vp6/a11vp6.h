/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * C interface to the vendored FFmpeg VP6 decoder.
 */

#ifndef A11_VP6_H
#define A11_VP6_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct A11Vp6 A11Vp6;

typedef struct A11Vp6Frame {
    int width;
    int height;
    const uint8_t *data[4];  /* Y, U, V, alpha (null without alpha) */
    int linesize[4];
    int key_frame;
} A11Vp6Frame;

/* extradata: the FLV crop byte (high nibble horizontal, low nibble vertical),
 * or null with size 0. Returns null on failure. */
A11Vp6 *a11_vp6_open(int has_alpha, const uint8_t *extradata, int extradata_size);

/* Decodes one FLV VP6 packet (for VP6A, the packet starts with the 3-byte
 * alpha offset). Returns 1 when *out holds a picture valid until the next
 * call, 0 when the packet produced none, a negative value on a bad packet. */
int a11_vp6_decode(A11Vp6 *ctx, const uint8_t *data, int size, A11Vp6Frame *out);

void a11_vp6_flush(A11Vp6 *ctx);
void a11_vp6_close(A11Vp6 *ctx);

#ifdef __cplusplus
}
#endif

#endif /* A11_VP6_H */
