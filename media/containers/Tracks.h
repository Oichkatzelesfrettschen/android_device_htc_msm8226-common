/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Track and packet tables the AVI and FLV parsers produce, and the readers
 * that turn them into decoder input.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "ByteSource.h"

namespace a11 {

// How a packet's stored bytes become decoder input.
enum class Transform : uint8_t {
    kVerbatim,
    // Length-prefixed H.264 NAL units to 00 00 00 01 start-code NAL units.
    kAvcAnnexB,
    // A chunk of MPEG audio Layer III split into whole frames.
    kMp3Frames,
};

struct Packet {
    uint64_t offset = 0;
    uint32_t size = 0;
    // Presentation time.
    int64_t timeUs = 0;
    bool key = false;
};

struct TrackInfo {
    bool video = false;
    std::string mime;
    std::vector<uint8_t> csd0;
    std::vector<uint8_t> csd1;
    int32_t width = 0;
    int32_t height = 0;
    int32_t channels = 0;
    int32_t sampleRate = 0;
    int32_t bitrate = 0;
    int32_t frameRate = 0;
    // AudioFormat encoding constant for audio/raw, zero otherwise.
    int32_t pcmEncoding = 0;
    int64_t durationUs = 0;
    size_t maxInput = 0;
    Transform transform = Transform::kVerbatim;
    int nalLengthSize = 4;
    std::vector<Packet> packets;
};

struct Container {
    std::vector<TrackInfo> tracks;
    int64_t durationUs = 0;
};

// Both return false when the file is not that container or holds no decodable track.
bool ParseAvi(ByteSource *source, Container *out);
bool ParseFlv(ByteSource *source, Container *out);

// Upper bound on one stored packet; larger packets are malformed.
constexpr uint32_t kMaxPacketBytes = 16u << 20;
// Upper bound on packets per track.
constexpr size_t kMaxPackets = 4u << 20;

// count * num * 1000000 / den without wrapping; false on overflow or den == 0.
inline bool UnitsToUs(uint64_t count, uint64_t num, uint64_t den, int64_t *us) {
    uint64_t scaled;
    if (den == 0 || __builtin_mul_overflow(count, num, &scaled)
            || __builtin_mul_overflow(scaled, UINT64_C(1000000), &scaled)) {
        return false;
    }
    scaled /= den;
    if (scaled > static_cast<uint64_t>(INT64_MAX)) {
        return false;
    }
    *us = static_cast<int64_t>(scaled);
    return true;
}

// Seek request, mirroring MediaTrackHelper::ReadOptions::SeekMode.
enum class SeekMode : uint8_t {
    kPreviousSync,
    kNextSync,
    kClosestSync,
    kClosest,
    // value is a frame (packet) index, not a time.
    kFrameIndex,
};

struct Sample {
    std::vector<uint8_t> data;
    int64_t timeUs = 0;
    bool key = false;
};

// Sequential reader over one track.
class TrackReader {
public:
    enum class Status : uint8_t { kOk, kEndOfStream, kIoError, kMalformed };

    TrackReader(ByteSource *source, const TrackInfo *track) : mSource(source), mTrack(track) {}

    // Positions the reader. Returns false when no packet satisfies the request
    // (a next-sync seek past the last key packet); the reader then reports the
    // end of the stream. *targetUs is the requested time for kClosest, else -1.
    bool seek(int64_t value, SeekMode mode, int64_t *targetUs);

    Status next(Sample *out);

private:
    Status readPacket(const Packet &packet, std::vector<uint8_t> *raw);
    Status nextMp3(Sample *out);

    ByteSource *mSource;
    const TrackInfo *mTrack;
    size_t mNext = 0;
    // kMp3Frames state: frames split from consumed chunks, a partial frame held
    // across a chunk boundary, and the running timestamp.
    std::vector<Sample> mPending;
    size_t mPendingPos = 0;
    std::vector<uint8_t> mCarry;
    int64_t mMp3AnchorUs = -1;
    uint64_t mMp3Samples = 0;
};

}  // namespace a11
