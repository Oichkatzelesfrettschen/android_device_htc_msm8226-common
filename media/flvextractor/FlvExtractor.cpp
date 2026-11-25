/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MediaExtractor plugin for FLV files carrying Sorenson Spark, On2 VP6 and
 * H.264 video with MP3 or AAC audio.
 */

#define LOG_TAG "FlvExtractor"

#include <string.h>

#include "PluginGlue.h"

namespace android {

namespace {

bool SniffFlv(DataSourceHelper *source, float *confidence) {
    uint8_t header[9];
    if (source->readAt(0, header, sizeof(header)) != static_cast<ssize_t>(sizeof(header))
            || memcmp(header, "FLV", 3) != 0 || header[3] != 1) {
        return false;
    }
    *confidence = 0.5f;
    return true;
}

const char *kExtensions[] = {
    "flv",
    nullptr,
};

}  // namespace

extern "C" {

__attribute__((visibility("default")))
ExtractorDef GETEXTRACTORDEF() {
    return {
        EXTRACTORDEF_VERSION,
        UUID("9d2a7f10-3c5e-4b88-a0d4-a11c0de0a5f3"),
        2,
        "FLV Extractor",
        {
            .v3 = {
                [](CDataSource *source, float *confidence, void **,
                        FreeMetaFunc *) -> CreatorFunc {
                    DataSourceHelper helper(source);
                    if (!SniffFlv(&helper, confidence)) {
                        return nullptr;
                    }
                    return [](CDataSource *source, void *) -> CMediaExtractor * {
                        return wrap(new ContainerExtractor(new DataSourceHelper(source),
                                a11::ParseFlv, "FlvExtractor", "video/x-flv"));
                    };
                },
                kExtensions,
            },
        },
    };
}

}  // extern "C"

}  // namespace android
