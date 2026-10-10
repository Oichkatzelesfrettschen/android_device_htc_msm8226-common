/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Drives the vendored FFmpeg VP6 decoder through its codec descriptors.
 */

#include <string.h>

#include "a11vp6.h"
#include "avcodec.h"
#include "codec_internal.h"

extern const FFCodec ff_vp6f_decoder;
extern const FFCodec ff_vp6a_decoder;

struct A11Vp6 {
    const FFCodec *codec;
    AVCodecContext avctx;
    AVFrame *frame;
    uint8_t *padded;
    size_t padded_capacity;
    int has_alpha;
};

static int execute2_serial(AVCodecContext *c,
                           int (*func)(AVCodecContext *c2, void *arg2, int jobnr, int threadnr),
                           void *arg2, int *ret, int count) {
    for (int i = 0; i < count; i++) {
        int r = func(c, arg2, i, 0);
        if (ret) {
            ret[i] = r;
        }
    }
    return 0;
}

A11Vp6 *a11_vp6_open(int has_alpha, const uint8_t *extradata, int extradata_size) {
    if (extradata_size < 0 || extradata_size > 1 || (extradata_size == 1 && !extradata)) {
        return NULL;
    }
    A11Vp6 *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->has_alpha = has_alpha;
    ctx->codec = has_alpha ? &ff_vp6a_decoder : &ff_vp6f_decoder;
    ctx->avctx.codec = &ctx->codec->p;
    ctx->avctx.codec_id = ctx->codec->p.id;
    ctx->avctx.priv_data = av_mallocz(ctx->codec->priv_data_size);
    ctx->avctx.execute2 = execute2_serial;
    ctx->avctx.skip_alpha = 0;
    ctx->avctx.skip_loop_filter = AVDISCARD_DEFAULT;
    if (extradata_size == 1) {
        ctx->avctx.extradata = av_mallocz(1 + AV_INPUT_BUFFER_PADDING_SIZE);
        if (ctx->avctx.extradata) {
            ctx->avctx.extradata[0] = extradata[0];
            ctx->avctx.extradata_size = 1;
        }
    }
    ctx->frame = av_frame_alloc();
    if (!ctx->avctx.priv_data || !ctx->frame || (extradata_size == 1 && !ctx->avctx.extradata) ||
        ctx->codec->init(&ctx->avctx) < 0) {
        if (ctx->avctx.priv_data && ctx->frame) {
            ctx->codec->close(&ctx->avctx);
        }
        av_free(ctx->avctx.priv_data);
        av_free(ctx->avctx.extradata);
        av_frame_free(&ctx->frame);
        av_free(ctx);
        return NULL;
    }
    return ctx;
}

int a11_vp6_decode(A11Vp6 *ctx, const uint8_t *data, int size, A11Vp6Frame *out) {
    if (!ctx || size <= 0 || !data) {
        return -1;
    }
    /* The range coder reads a few bytes past the packet. */
    if ((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE > ctx->padded_capacity) {
        av_free(ctx->padded);
        ctx->padded_capacity = (size_t)size + AV_INPUT_BUFFER_PADDING_SIZE;
        ctx->padded = av_malloc(ctx->padded_capacity);
        if (!ctx->padded) {
            ctx->padded_capacity = 0;
            return -1;
        }
    }
    memcpy(ctx->padded, data, size);
    memset(ctx->padded + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    AVPacket pkt = {ctx->padded, size};
    av_frame_unref(ctx->frame);
    int got = 0;
    int ret = ctx->codec->cb.decode(&ctx->avctx, ctx->frame, &got, &pkt);
    if (ret < 0) {
        return ret;
    }
    if (!got) {
        return 0;
    }
    out->width = ctx->avctx.width;
    out->height = ctx->avctx.height;
    for (int i = 0; i < 4; i++) {
        out->data[i] = ctx->frame->data[i];
        out->linesize[i] = ctx->frame->linesize[i];
    }
    out->key_frame = !!(ctx->frame->flags & AV_FRAME_FLAG_KEY);
    return 1;
}

void a11_vp6_flush(A11Vp6 *ctx) {
    (void)ctx; /* VP6 keeps reference frames only; a key frame resets them. */
}

void a11_vp6_close(A11Vp6 *ctx) {
    if (!ctx) {
        return;
    }
    ctx->codec->close(&ctx->avctx);
    av_frame_free(&ctx->frame);
    av_free(ctx->avctx.priv_data);
    av_free(ctx->avctx.extradata);
    av_free(ctx->padded);
    av_free(ctx);
}
