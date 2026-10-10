/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bitstream header parsers shared by the container readers: MPEG audio frame
 * headers, AAC AudioSpecificConfig, H.264 avcC and SPS, Sorenson Spark and
 * On2 VP6 picture headers.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace a11 {

struct Mp3Header {
    int32_t sampleRate = 0;
    int32_t channels = 0;
    int32_t bitrate = 0;
    uint32_t frameBytes = 0;
    uint32_t samplesPerFrame = 0;
};

// Parses an MPEG-1/2/2.5 Layer III frame header at p (needs 4 bytes).
bool ParseMp3Header(const uint8_t *p, size_t size, Mp3Header *out);

struct AacConfig {
    int32_t sampleRate = 0;
    int32_t channels = 0;
    int objectType = 0;
};

// Parses an AudioSpecificConfig, including the explicit 24-bit sampling
// frequency (index 15) and the channel-configuration to channel-count map.
// A channel configuration of 0 (a program config element) is rejected.
bool ParseAacConfig(const uint8_t *data, size_t size, AacConfig *out);

struct AvcConfig {
    int nalLengthSize = 4;
    // Annex B (00 00 00 01 prefixed) parameter sets.
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    int32_t width = 0;
    int32_t height = 0;
};

// Parses an AVCDecoderConfigurationRecord (ISO/IEC 14496-15) and the first SPS.
bool ParseAvcConfig(const uint8_t *data, size_t size, AvcConfig *out);

// Cropped luma dimensions of an SPS NAL unit (header byte included).
bool ParseSpsDimensions(const uint8_t *nal, size_t size, int32_t *width, int32_t *height);

// Rewrites length-prefixed NAL units as 00 00 00 01 prefixed ones. False when
// a length runs past the data or is zero.
bool AvcToAnnexB(const uint8_t *data, size_t size, int lengthSize, std::vector<uint8_t> *out);

// Sorenson Spark (FLV1) picture header: frame size.
bool ParseSparkSize(const uint8_t *data, size_t size, int32_t *width, int32_t *height);

// On2 VP6 key frame header (after the FLV adjustment byte): coded size in
// pixels minus the horizontal and vertical crop of the FLV tag.
bool ParseVp6KeyFrame(const uint8_t *data, size_t size, int hAdjust, int vAdjust,
        int32_t *width, int32_t *height);

}  // namespace a11
