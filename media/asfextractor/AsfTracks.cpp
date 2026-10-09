/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mapping from ASF stream properties to Android track formats and samples.
 */

#include "AsfTracks.h"

#include <algorithm>
#include <cstddef>

namespace asf {

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16)
            | ((uint32_t)(uint8_t)d << 24);
}

// MIME types follow the Qualcomm media definitions the OMX component roles
// are keyed on.
constexpr const char *kMimeWma = "audio/x-ms-wma";
constexpr const char *kMimeWmaPro = "audio/x-ms-wma-pro";
constexpr const char *kMimeWmaLossless = "audio/x-ms-wma-lossless";
constexpr const char *kMimeWmv = "video/x-ms-wmv";
constexpr const char *kMimeWvc1 = "video/wvc1";
constexpr const char *kMimeMpeg4 = "video/mp4v-es";
constexpr const char *kMimeMp3 = "audio/mpeg";

bool DescribeAudio(const StreamInfo &stream, TrackFormat *format) {
    switch (stream.formatTag) {
        case 0x0161:  // WMA 2 (WMA 7, 8, 9 standard)
            format->mime = kMimeWma;
            break;
        case 0x0162:  // WMA 9 Professional
        case 0x0166:  // WMA 10 Professional
        case 0x0167:
            format->mime = kMimeWmaPro;
            break;
        case 0x0163:  // WMA 9 Lossless
            format->mime = kMimeWmaLossless;
            break;
        case 0x0055:  // MPEG-1 Layer III
            format->mime = kMimeMp3;
            break;
        default:
            // WMA 1 (0x0160): libOmxWmaDec labels every standard stream 0x0161.
            // WMA Voice (0x000A) and others have no decoder on the device.
            return false;
    }
    if (stream.channels == 0 || stream.sampleRate == 0) {
        return false;
    }
    format->audio = true;
    if (format->mime != kMimeMp3) {
        // The WMA decoders read WAVEFORMATEX and its codec options from csd-0.
        format->csd0 = stream.waveFormat;
    }
    format->channels = stream.channels;
    format->sampleRate = (int32_t)std::min<uint32_t>(stream.sampleRate, INT32_MAX);
    format->bitrate = (int32_t)std::min<uint64_t>((uint64_t)stream.avgBytesPerSec * 8, INT32_MAX);
    return true;
}

// Offset of the first 00 00 01 start code, or the size when there is none.
size_t FindStartCode(const std::vector<uint8_t> &data) {
    for (size_t i = 0; i + 3 <= data.size(); ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            return i;
        }
    }
    return data.size();
}

bool DescribeVideo(const StreamInfo &stream, TrackFormat *format) {
    switch (stream.fourcc) {
        case FourCC('W', 'M', 'V', '3'):
            // Simple and main profile: the codec data is STRUCT_C, which the
            // Venus decoder takes as its codec configuration.
            if (stream.codecData.size() < 4) {
                return false;
            }
            format->mime = kMimeWmv;
            format->csd0 = stream.codecData;
            break;
        case FourCC('W', 'V', 'C', '1'):
        case FourCC('W', 'M', 'V', 'A'): {
            // Advanced profile: the codec data is a byte of ASF binding
            // followed by the sequence header and entry point start codes.
            size_t start = FindStartCode(stream.codecData);
            if (start >= stream.codecData.size()) {
                return false;
            }
            format->mime = kMimeWvc1;
            format->csd0.assign(stream.codecData.begin() + static_cast<std::ptrdiff_t>(start),
                    stream.codecData.end());
            format->layout = SampleLayout::kVc1FrameStartCode;
            break;
        }
        case FourCC('M', '4', 'S', '2'):
        case FourCC('M', 'P', '4', 'S'):
        case FourCC('m', '4', 's', '2'):
        case FourCC('m', 'p', '4', 's'):
            // MPEG-4 Part 2: the codec data is the visual object sequence
            // and video object layer header.
            format->mime = kMimeMpeg4;
            format->csd0 = stream.codecData;
            break;
        default:
            // WMV1 and WMV2 (Windows Media Video 7 and 8) have no decoder.
            return false;
    }
    if (stream.width == 0 || stream.height == 0 || stream.width > 4096
            || stream.height > 4096) {
        return false;
    }
    format->width = (int32_t)stream.width;
    format->height = (int32_t)stream.height;
    format->bitrate = (int32_t)std::min<uint32_t>(stream.bitrate, INT32_MAX);
    if (stream.avgTimePerFrame100ns > 0) {
        uint64_t fps = (10000000ull + stream.avgTimePerFrame100ns / 2)
                / stream.avgTimePerFrame100ns;
        format->frameRate = (int32_t)std::min<uint64_t>(fps, 240);
    }
    return true;
}

}  // namespace

bool DescribeStream(const AsfFile &file, const StreamInfo &stream, TrackFormat *format) {
    if (stream.encrypted) {
        return false;
    }
    bool ok = false;
    if (stream.kind == StreamKind::kAudio) {
        ok = DescribeAudio(stream, format);
    } else if (stream.kind == StreamKind::kVideo) {
        ok = DescribeVideo(stream, format);
    }
    if (!ok) {
        return false;
    }
    format->durationUs = file.durationUs();
    size_t largest = std::max<size_t>(file.packetSize(), stream.maxObjectSize);
    if (format->audio) {
        largest = std::max<size_t>(largest, stream.blockAlign);
        if (stream.spreadSpan > 1) {
            largest = std::max<size_t>(largest,
                    (size_t)stream.spreadSpan * stream.spreadPacketLength);
        }
    } else if (stream.maxObjectSize == 0) {
        // Without an Extended Stream Properties bound, half an uncompressed
        // 4:2:0 picture covers a compressed frame; ACodec sizes every Venus
        // input buffer from this value.
        largest = std::max<size_t>(largest, (size_t)format->width * format->height * 3 / 4);
    }
    // Four bytes cover the frame start code PrepareSample may insert.
    format->maxInputSize = largest + 4;
    return true;
}

std::vector<uint8_t> PrepareSample(const TrackFormat &format, std::vector<uint8_t> &&data) {
    if (format.layout == SampleLayout::kVc1FrameStartCode
            && !(data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)) {
        static const uint8_t kFrameStartCode[4] = {0x00, 0x00, 0x01, 0x0D};
        data.insert(data.begin(), kFrameStartCode, kFrameStartCode + 4);
    }
    return std::move(data);
}

}  // namespace asf
