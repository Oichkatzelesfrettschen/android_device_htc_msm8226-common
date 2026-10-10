/*
 * Minimal replacement for FFmpeg's libavutil/frame.h: a reference-counted
 * planar picture with the fields the vendored VP6 decoder uses.
 */

#ifndef AVUTIL_FRAME_H
#define AVUTIL_FRAME_H

#include <stdint.h>

#define AV_NUM_DATA_POINTERS 4
#define AV_FRAME_FLAG_KEY (1 << 1)
#define AV_FRAME_FLAG_INTERLACED (1 << 3)

enum AVPictureType {
    AV_PICTURE_TYPE_NONE = 0,
    AV_PICTURE_TYPE_I,
    AV_PICTURE_TYPE_P,
};

struct AVFrameStorage;

typedef struct AVFrame {
    uint8_t *data[AV_NUM_DATA_POINTERS];
    int linesize[AV_NUM_DATA_POINTERS];
    int width, height;
    int format;
    int flags;
    enum AVPictureType pict_type;
    struct AVFrameStorage *storage;
} AVFrame;

AVFrame *av_frame_alloc(void);
void av_frame_free(AVFrame **frame);
void av_frame_unref(AVFrame *frame);
int av_frame_ref(AVFrame *dst, const AVFrame *src);
int av_frame_replace(AVFrame *dst, const AVFrame *src);

#endif /* AVUTIL_FRAME_H */
