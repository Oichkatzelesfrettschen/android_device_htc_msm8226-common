/* Minimal replacement for FFmpeg's libavcodec/decode.h. */
#ifndef AVCODEC_DECODE_H
#define AVCODEC_DECODE_H

#include "avcodec.h"

int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags);
int ff_set_dimensions(AVCodecContext *s, int width, int height);

#endif /* AVCODEC_DECODE_H */
