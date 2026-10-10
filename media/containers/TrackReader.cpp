/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sequential track reader: seek positioning and packet-to-sample transforms.
 */

#include "Codecs.h"
#include "Tracks.h"

#include <algorithm>

namespace a11 {

bool TrackReader::seek(int64_t value, SeekMode mode, int64_t *targetUs) {
    const std::vector<Packet> &p = mTrack->packets;
    const size_t n = p.size();
    *targetUs = -1;
    mPending.clear();
    mPendingPos = 0;
    mCarry.clear();
    mMp3AnchorUs = -1;
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
    const std::vector<Packet> &p = mTrack->packets;
    while (mPendingPos >= mPending.size()) {
        mPending.clear();
        mPendingPos = 0;
        if (mNext >= p.size()) {
            return Status::kEndOfStream;
        }
        const Packet &packet = p[mNext++];
        std::vector<uint8_t> raw;
        Status st = readPacket(packet, &raw);
        if (st != Status::kOk) {
            return st;
        }
        // A packet that starts on a frame boundary (nothing carried) re-anchors
        // frame timing at its container timestamp, so timestamp gaps survive;
        // a carried partial frame keeps the running sample clock.
        if (mMp3AnchorUs < 0 || mCarry.empty()) {
            mMp3AnchorUs = packet.timeUs;
            mMp3Samples = 0;
        }
        mCarry.insert(mCarry.end(), raw.begin(), raw.end());
        size_t pos = 0;
        while (pos + 4 <= mCarry.size()) {
            Mp3Header h;
            if (!ParseMp3Header(mCarry.data() + pos, mCarry.size() - pos, &h)) {
                ++pos;
                continue;
            }
            if (pos + h.frameBytes > mCarry.size()) {
                break;
            }
            Sample s;
            s.data.assign(mCarry.begin() + static_cast<std::ptrdiff_t>(pos),
                    mCarry.begin() + static_cast<std::ptrdiff_t>(pos + h.frameBytes));
            int64_t offsetUs = 0;
            if (!UnitsToUs(mMp3Samples, 1, static_cast<uint64_t>(h.sampleRate), &offsetUs)) {
                return Status::kMalformed;
            }
            s.timeUs = mMp3AnchorUs + offsetUs;
            s.key = true;
            mMp3Samples += h.samplesPerFrame;
            mPending.push_back(std::move(s));
            pos += h.frameBytes;
        }
        mCarry.erase(mCarry.begin(), mCarry.begin() + static_cast<std::ptrdiff_t>(pos));
        if (mCarry.size() > (64u << 10)) {
            mCarry.clear();  // no frame sync in 64 KiB: resynchronize on the next chunk
        }
    }
    *out = std::move(mPending[mPendingPos++]);
    return Status::kOk;
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
