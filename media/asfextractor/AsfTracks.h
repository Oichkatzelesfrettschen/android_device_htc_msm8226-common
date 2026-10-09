/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mapping from ASF stream properties to Android track formats and samples.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "AsfParser.h"

namespace asf {

enum class SampleLayout : uint8_t {
    kVerbatim,
    // SMPTE 421M advanced profile frames: ASF stores a frame without its
    // frame start code, the decoder's start-code parser needs it.
    kVc1FrameStartCode,
};

struct TrackFormat {
    std::string mime;
    bool audio = false;
    std::vector<uint8_t> csd0;
    int32_t channels = 0;
    int32_t sampleRate = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t bitrate = 0;
    int32_t frameRate = 0;
    int64_t durationUs = 0;
    size_t maxInputSize = 0;
    SampleLayout layout = SampleLayout::kVerbatim;
};

// Returns false for a stream with no decoder path on the device.
bool DescribeStream(const AsfFile &file, const StreamInfo &stream, TrackFormat *format);

std::vector<uint8_t> PrepareSample(const TrackFormat &format, std::vector<uint8_t> &&data);

}  // namespace asf
