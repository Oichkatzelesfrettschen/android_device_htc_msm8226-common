/*
 * Minimal replacement for FFmpeg's libavcodec/codec_internal.h: the codec
 * descriptor the decoder source fills in. The wrapper calls its entry points.
 */

#ifndef AVCODEC_CODEC_INTERNAL_H
#define AVCODEC_CODEC_INTERNAL_H

#include "avcodec.h"

#define FF_CODEC_CAP_INIT_CLEANUP (1 << 1)
#define CODEC_LONG_NAME(str) .p.long_name = (str)
#define FF_CODEC_DECODE_CB(func) .cb.decode = (func)

typedef struct FFCodec {
    AVCodec p;
    int priv_data_size;
    int (*init)(AVCodecContext *);
    int (*close)(AVCodecContext *);
    union {
        int (*decode)(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt);
    } cb;
    int caps_internal;
} FFCodec;

#endif /* AVCODEC_CODEC_INTERNAL_H */
