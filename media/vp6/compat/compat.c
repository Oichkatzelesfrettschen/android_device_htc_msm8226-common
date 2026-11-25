/*
 * Services the vendored FFmpeg VP6 decoder expects from libavutil and
 * libavcodec: allocation, logging, CPU flags, reference-counted frames and
 * frame buffers.
 */

#include <stdarg.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif
#if defined(__linux__)
#include <sys/auxv.h>
#endif

#include "avcodec.h"
#include "decode.h"
#include "libavutil/cpu.h"
#include "libavutil/mem.h"

void *av_malloc(size_t size) {
    void *p = NULL;
    if (size == 0 || posix_memalign(&p, 32, size) != 0) {
        return NULL;
    }
    return p;
}

void *av_mallocz(size_t size) {
    void *p = av_malloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void *av_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return av_malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    /* A fresh aligned block; the old one stays valid when allocation fails. */
    void *fresh = av_malloc(size);
    if (!fresh) {
        return NULL;
    }
    size_t old = malloc_usable_size(ptr);
    memcpy(fresh, ptr, old < size ? old : size);
    free(ptr);
    return fresh;
}

void *av_calloc(size_t nmemb, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(nmemb, size, &total)) {
        return NULL;
    }
    return av_mallocz(total);
}

void av_free(void *ptr) { free(ptr); }

void av_freep(void *arg) {
    void **pp = (void **)arg;
    void *val = *pp;
    *pp = NULL;
    av_free(val);
}

void av_log(void *avcl, int level, const char *fmt, ...) {
    (void)avcl;
    va_list ap;
    va_start(ap, fmt);
#if defined(__ANDROID__)
    __android_log_vprint(level <= AV_LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_DEBUG, "a11vp6",
                         fmt, ap);
#else
    if (level <= AV_LOG_ERROR) {
        vfprintf(stderr, fmt, ap);
    }
#endif
    va_end(ap);
}

int av_get_cpu_flags(void) {
    int flags = 0;
#if defined(__arm__) && defined(__linux__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if (hwcap & (1UL << 12)) /* HWCAP_NEON */
        flags |= AV_CPU_FLAG_NEON | AV_CPU_FLAG_VFPV3 | AV_CPU_FLAG_VFP;
    if (hwcap & (1UL << 6)) /* HWCAP_VFP */
        flags |= AV_CPU_FLAG_VFP;
    if (hwcap & (1UL << 13)) /* HWCAP_VFPv3 */
        flags |= AV_CPU_FLAG_VFPV3;
    flags |= AV_CPU_FLAG_ARMV5TE | AV_CPU_FLAG_ARMV6 | AV_CPU_FLAG_ARMV6T2 | AV_CPU_FLAG_SETEND;
#endif
    return flags;
}

struct AVFrameStorage {
    uint8_t *base;
    int refs;
};

AVFrame *av_frame_alloc(void) { return av_mallocz(sizeof(AVFrame)); }

void av_frame_unref(AVFrame *frame) {
    if (!frame) {
        return;
    }
    if (frame->storage && --frame->storage->refs == 0) {
        av_free(frame->storage->base);
        av_free(frame->storage);
    }
    memset(frame, 0, sizeof(*frame));
}

void av_frame_free(AVFrame **frame) {
    if (!frame || !*frame) {
        return;
    }
    av_frame_unref(*frame);
    av_freep(frame);
}

int av_frame_ref(AVFrame *dst, const AVFrame *src) {
    *dst = *src;
    if (dst->storage) {
        dst->storage->refs++;
    }
    return 0;
}

int av_frame_replace(AVFrame *dst, const AVFrame *src) {
    if (dst == src) {
        return 0;
    }
    av_frame_unref(dst);
    return src->storage ? av_frame_ref(dst, src) : 0;
}

int ff_set_dimensions(AVCodecContext *s, int width, int height) {
    if (width < 0 || height < 0 || (long long)width * height > 16384LL * 16384) {
        return AVERROR(EINVAL);
    }
    s->coded_width = width;
    s->coded_height = height;
    s->width = width;
    s->height = height;
    return 0;
}

int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags) {
    (void)flags;
    const int w = avctx->coded_width ? avctx->coded_width : avctx->width;
    const int h = avctx->coded_height ? avctx->coded_height : avctx->height;
    if (w <= 0 || h <= 0) {
        return AVERROR(EINVAL);
    }
    const int alpha = avctx->pix_fmt == AV_PIX_FMT_YUVA420P;
    const int aw = (w + 15) & ~15;
    const int ah = (h + 15) & ~15;
    const int stride[4] = {(aw + 63) & ~31, (aw / 2 + 63) & ~31, (aw / 2 + 63) & ~31,
                           (aw + 63) & ~31};
    const int rows[4] = {ah + 2, ah / 2 + 2, ah / 2 + 2, ah + 2};
    size_t total = 0, offset[4];
    const int planes = alpha ? 4 : 3;
    for (int i = 0; i < planes; i++) {
        offset[i] = total;
        total += (size_t)stride[i] * rows[i];
    }
    /* The caller sets flags such as the key-frame bit before asking for the
     * buffer, so only the picture storage is released. */
    if (frame->storage && --frame->storage->refs == 0) {
        av_free(frame->storage->base);
        av_free(frame->storage);
    }
    frame->storage = av_mallocz(sizeof(*frame->storage));
    if (!frame->storage) {
        return AVERROR(ENOMEM);
    }
    frame->storage->base = av_mallocz(total);
    if (!frame->storage->base) {
        av_freep(&frame->storage);
        return AVERROR(ENOMEM);
    }
    frame->storage->refs = 1;
    for (int i = 0; i < planes; i++) {
        frame->data[i] = frame->storage->base + offset[i];
        frame->linesize[i] = stride[i];
    }
    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->format = avctx->pix_fmt;
    return 0;
}

void *av_malloc_array(size_t nmemb, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(nmemb, size, &total)) {
        return NULL;
    }
    return av_malloc(total);
}

void *av_realloc_f(void *ptr, size_t nelem, size_t elsize) {
    size_t total;
    if (__builtin_mul_overflow(nelem, elsize, &total)) {
        av_free(ptr);
        return NULL;
    }
    void *r = av_realloc(ptr, total);
    if (!r && total) {
        av_free(ptr);
    }
    return r;
}

int av_reallocp_array(void *ptr, size_t nmemb, size_t size) {
    void **pp = (void **)ptr;
    void *val = av_realloc_f(*pp, nmemb, size);
    *pp = val;
    if (!val && nmemb && size) {
        return AVERROR(ENOMEM);
    }
    return 0;
}

void avpriv_request_sample(void *avc, const char *fmt, ...) {
    (void)avc;
    (void)fmt;
}
