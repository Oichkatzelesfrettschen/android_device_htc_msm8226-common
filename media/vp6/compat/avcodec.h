/*
 * Minimal replacement for FFmpeg's libavcodec/avcodec.h: the codec context,
 * packet and codec-description fields the vendored VP6 decoder reads.
 */

#ifndef AVCODEC_AVCODEC_H
#define AVCODEC_AVCODEC_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixfmt.h"
#include "libavcodec/defs.h"

#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define AV_GET_BUFFER_FLAG_REF (1 << 0)

#define AV_CODEC_FLAG_GRAY (1 << 13)
#define AV_CODEC_CAP_DR1 (1 << 1)
#define AV_CODEC_CAP_SLICE_THREADS (1 << 13)

enum AVMediaType { AVMEDIA_TYPE_UNKNOWN = -1, AVMEDIA_TYPE_VIDEO = 0 };

enum AVCodecID {
    AV_CODEC_ID_NONE = 0,
    AV_CODEC_ID_VP5,
    AV_CODEC_ID_VP6,
    AV_CODEC_ID_VP6F,
    AV_CODEC_ID_VP6A,
};


typedef struct AVCodec {
    const char *name;
    const char *long_name;
    enum AVMediaType type;
    enum AVCodecID id;
    int capabilities;
} AVCodec;

typedef struct AVPacket {
    uint8_t *data;
    int size;
} AVPacket;

typedef struct AVCodecContext AVCodecContext;

struct AVCodecContext {
    const AVCodec *codec;
    enum AVCodecID codec_id;
    void *priv_data;
    int width, height;
    int coded_width, coded_height;
    enum AVPixelFormat pix_fmt;
    int flags;
    int error_concealment;
    enum AVDiscard skip_loop_filter;
    int skip_alpha;
    uint8_t *extradata;
    int extradata_size;
    int (*execute2)(AVCodecContext *c,
                    int (*func)(AVCodecContext *c2, void *arg2, int jobnr, int threadnr),
                    void *arg2, int *ret, int count);
};

#endif /* AVCODEC_AVCODEC_H */
