/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sequential track reader: seek positioning and packet-to-sample transforms.
 */

#include "Codecs.h"
#include "Tracks.h"

#include <algorithm>
#include <string.h>

namespace a11 {

bool TrackReader::seek(int64_t value, SeekMode mode, int64_t *targetUs) {
    const std::vector<Packet> &p = mTrack->packets;
    const size_t n = p.size();
    *targetUs = -1;
    mMp3Begin = 0;
    mMp3End = 0;
    mMp3PacketPos = 0;
    mMp3Anchored = false;
    mMp3AnchorUs = 0;
    mMp3Samples = 0;
    mNext = n;
    if (n == 0) {
        return false;
    }
    auto previousKey = [&](size_t from) {
        while (from > 0 && !p[from].key) {
            --from;
        }
        return from;
    };
    if (mode == SeekMode::kFrameIndex) {
        const size_t index = value < 0 ? 0 : std::min<uint64_t>(static_cast<uint64_t>(value), n - 1);
        *targetUs = p[index].timeUs;
        mNext = previousKey(index);
        return true;
    }
    // Last packet at or before the time; the first packet when none is.
    size_t at = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i].timeUs <= value) {
            at = i;
        }
    }
    // First sync packet at or after the time.
    size_t after = n;
    for (size_t i = 0; i < n; ++i) {
        if (p[i].key && p[i].timeUs >= value) {
            after = i;
            break;
        }
    }
    const size_t before = previousKey(at);
    switch (mode) {
        case SeekMode::kNextSync:
            if (after == n) {
                return false;
            }
            mNext = after;
            return true;
        case SeekMode::kClosestSync:
            // Distances are taken in unsigned arithmetic: a request at the
            // int64_t extremes has no representable signed difference.
            if (after != n &&
                static_cast<uint64_t>(p[after].timeUs) - static_cast<uint64_t>(value) <
                        static_cast<uint64_t>(value) - static_cast<uint64_t>(p[before].timeUs)) {
                mNext = after;
            } else {
                mNext = before;
            }
            return true;
        case SeekMode::kClosest:
            *targetUs = value;
            mNext = before;
            return true;
        default:
            mNext = before;
            return true;
    }
}

TrackReader::Status TrackReader::readPacket(const Packet &packet, std::vector<uint8_t> *raw) {
    if (packet.size > kMaxPacketBytes) {
        return Status::kMalformed;
    }
    raw->resize(packet.size);
    if (!ReadFully(mSource, packet.offset, raw->data(), packet.size)) {
        return Status::kIoError;
    }
    return Status::kOk;
}

TrackReader::Status TrackReader::nextMp3(Sample *out) {
    const std::vector<Packet> &packets = mTrack->packets;
    while (true) {
        while (mMp3End - mMp3Begin >= 4) {
            Mp3Header header;
            if (!ParseMp3Header(mMp3Buffer.data() + mMp3Begin,
                    mMp3End - mMp3Begin, &header)) {
                ++mMp3Begin;
                continue;
            }
            if (header.frameBytes > mMp3End - mMp3Begin) {
                break;
            }
            int64_t offsetUs;
            int64_t timeUs;
            if (!UnitsToUs(mMp3Samples, 1, static_cast<uint64_t>(header.sampleRate), &offsetUs)
                    || __builtin_add_overflow(mMp3AnchorUs, offsetUs, &timeUs)) {
                return Status::kMalformed;
            }
            out->data.assign(mMp3Buffer.begin() + static_cast<std::ptrdiff_t>(mMp3Begin),
                    mMp3Buffer.begin() + static_cast<std::ptrdiff_t>(mMp3Begin + header.frameBytes));
            out->timeUs = timeUs;
            out->key = true;
            mMp3Samples += header.samplesPerFrame;
            mMp3Begin += header.frameBytes;
            return Status::kOk;
        }
        if (mNext >= packets.size()) {
            mMp3Begin = 0;
            mMp3End = 0;
            return Status::kEndOfStream;
        }
        // Scanning leaves at most three unsynchronized bytes or an incomplete
        // frame. Compact that tail only when another source read is needed.
        const size_t carried = mMp3End - mMp3Begin;
        if (carried != 0 && mMp3Begin != 0) {
            memmove(mMp3Buffer.data(), mMp3Buffer.data() + mMp3Begin, carried);
        }
        mMp3Begin = 0;
        mMp3End = carried;
        const Packet &packet = packets[mNext];
        if (packet.size > kMaxPacketBytes) {
            return Status::kMalformed;
        }
        // A new packet re-anchors the clock only at an empty frame boundary.
        // Refills inside one packet and carried partial frames keep the clock.
        if (mMp3PacketPos == 0 && (!mMp3Anchored || carried == 0)) {
            mMp3AnchorUs = packet.timeUs;
            mMp3Samples = 0;
            mMp3Anchored = true;
        }
        const size_t bytes = std::min<size_t>(packet.size - mMp3PacketPos,
                mMp3Buffer.size() - carried);
        if (bytes != 0) {
            uint64_t offset;
            if (__builtin_add_overflow(packet.offset, mMp3PacketPos, &offset)
                    || !ReadFully(mSource, offset, mMp3Buffer.data() + carried, bytes)) {
                return Status::kIoError;
            }
        }
        mMp3End += bytes;
        mMp3PacketPos += static_cast<uint32_t>(bytes);
        if (mMp3PacketPos == packet.size) {
            ++mNext;
            mMp3PacketPos = 0;
        }
    }
}

TrackReader::Status TrackReader::next(Sample *out) {
    if (mTrack->transform == Transform::kMp3Frames) {
        return nextMp3(out);
    }
    const std::vector<Packet> &p = mTrack->packets;
    if (mNext >= p.size()) {
        return Status::kEndOfStream;
    }
    const Packet &packet = p[mNext++];
    std::vector<uint8_t> raw;
    Status st = readPacket(packet, &raw);
    if (st != Status::kOk) {
        return st;
    }
    out->timeUs = packet.timeUs;
    out->key = packet.key;
    if (mTrack->transform == Transform::kAvcAnnexB) {
        if (!AvcToAnnexB(raw.data(), raw.size(), mTrack->nalLengthSize, &out->data)) {
            return Status::kMalformed;
        }
    } else {
        out->data = std::move(raw);
    }
    return Status::kOk;
}

}  // namespace a11
