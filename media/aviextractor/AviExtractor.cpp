/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MediaExtractor plugin for AVI files carrying DivX, MPEG-4 Part 2, H.264,
 * MP3, AAC and PCM streams.
 */

#define LOG_TAG "AviExtractor"

#include <string.h>

#include "PluginGlue.h"

namespace android {

namespace {

bool SniffAvi(DataSourceHelper *source, float *confidence) {
    uint8_t header[12];
    if (source->readAt(0, header, sizeof(header)) != static_cast<ssize_t>(sizeof(header))
            || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "AVI ", 4) != 0) {
        return false;
    }
    *confidence = 0.5f;
    return true;
}

const char *kExtensions[] = {
    "avi",
    nullptr,
};

}  // namespace

extern "C" {

__attribute__((visibility("default")))
ExtractorDef GETEXTRACTORDEF() {
    return {
        EXTRACTORDEF_VERSION,
        UUID("0c4e6b52-7f3a-4d21-8b6e-a11c0de0a5f2"),
        2,
        "AVI Extractor",
        {
            .v3 = {
                [](CDataSource *source, float *confidence, void **,
                        FreeMetaFunc *) -> CreatorFunc {
                    DataSourceHelper helper(source);
                    if (!SniffAvi(&helper, confidence)) {
                        return nullptr;
                    }
                    return [](CDataSource *source, void *) -> CMediaExtractor * {
                        return wrap(new ContainerExtractor(new DataSourceHelper(source),
                                a11::ParseAvi, "AviExtractor", "video/avi"));
                    };
                },
                kExtensions,
            },
        },
    };
}

}  // extern "C"

}  // namespace android
