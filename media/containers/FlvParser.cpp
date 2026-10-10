/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * FLV demuxer tables: Sorenson Spark, On2 VP6/VP6A and H.264 video, MP3 and
 * AAC audio.
 */

#include <string.h>

#include <algorithm>
#include <vector>

#include "Codecs.h"
#include "Tracks.h"

namespace a11 {

namespace {

constexpr const char *kMimeSpark = "video/x-flv1";
constexpr const char *kMimeVp6 = "video/x-vnd.on2.vp6";
constexpr const char *kMimeAvc = "video/avc";
constexpr const char *kMimeMp3 = "audio/mpeg";
constexpr const char *kMimeAac = "audio/mp4a-latm";

constexpr uint8_t kTagAudio = 8;
constexpr uint8_t kTagVideo = 9;
constexpr size_t kMaxAscBytes = 64;

constexpr uint8_t kCodecSpark = 2;
constexpr uint8_t kCodecVp6 = 4;
constexpr uint8_t kCodecVp6Alpha = 5;
constexpr uint8_t kCodecAvc = 7;
constexpr uint8_t kSoundMp3 = 2;
constexpr uint8_t kSoundMp3Rate8k = 14;
constexpr uint8_t kSoundAac = 10;
constexpr uint8_t kFrameKey = 1;
constexpr uint8_t kFrameInfo = 5;  // video info/command frame: no picture

uint32_t Be24(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

uint32_t Be32(const uint8_t *p) { return (Be24(p) << 8) | p[3]; }

class FlvFile {
public:
    bool parse(ByteSource *source, Container *out) {
        mSource = source;
        uint8_t head[13];
        if (!ReadFully(source, 0, head, sizeof(head)) || memcmp(head, "FLV", 3) != 0 || head[3] != 1) {
            return false;
        }
        const uint32_t dataOffset = Be32(head + 5);
        const int64_t total = source->size();
        if (dataOffset < 9 || total <= 0) {
            return false;
        }
        mVideo.video = true;
        uint64_t pos = static_cast<uint64_t>(dataOffset) + 4;  // skip PreviousTagSize0
        size_t tags = 0;
        while (pos + 11 <= static_cast<uint64_t>(total) && tags++ < 4 * kMaxPackets) {
            uint8_t th[11];
            if (!ReadFully(source, pos, th, sizeof(th))) {
                break;
            }
            const uint8_t type = th[0] & 0x1F;
            const uint32_t size = Be24(th + 1);
            const int64_t timeMs = static_cast<int64_t>(Be24(th + 4) | (static_cast<uint32_t>(th[7]) << 24));
            const uint64_t body = pos + 11;
            if (size > kMaxPacketBytes || body + size > static_cast<uint64_t>(total)) {
                break;
            }
            if (type == kTagVideo && size >= 2) {
                addVideo(body, size, timeMs);
            } else if (type == kTagAudio && size >= 2) {
                addAudio(body, size, timeMs);
            }
            pos = body + size + 4;  // PreviousTagSize
        }
        return finish(out);
    }

private:
    void addVideo(uint64_t body, uint32_t size, int64_t timeMs) {
        uint8_t h[32];
        const size_t n = std::min<size_t>(size, sizeof(h));
        if (!ReadFully(mSource, body, h, n)) {
            return;
        }
        const uint8_t codec = h[0] & 0x0F;
        const uint8_t frameType = h[0] >> 4;
        if (frameType == kFrameInfo) {
            return;
        }
        if (mVideoCodec == 0) {
            if (codec != kCodecSpark && codec != kCodecVp6 && codec != kCodecVp6Alpha && codec != kCodecAvc) {
                return;
            }
            mVideoCodec = codec;
        } else if (mVideoCodec != codec) {
            return;
        }
        Packet p;
        p.key = frameType == kFrameKey;
        switch (codec) {
            case kCodecSpark: {
                if (mVideo.width == 0
                        && !ParseSparkSize(h + 1, n - 1, &mVideo.width, &mVideo.height)) {
                    return;  // wait for a picture that carries a header
                }
                mVideo.mime = kMimeSpark;
                p.offset = body + 1;
                p.size = size - 1;
                p.timeUs = timeMs * 1000;
                break;
            }
            case kCodecVp6:
            case kCodecVp6Alpha: {
                // Byte 1 holds the horizontal (high nibble) and vertical crop;
                // VP6A then carries a 3-byte offset to the alpha plane, which
                // stays in the packet for the decoder.
                const size_t lead = codec == kCodecVp6Alpha ? 5 : 2;
                if (size < lead + 6 || n <= lead) {
                    return;
                }
                if (mVideo.width == 0) {
                    const int adj = h[1];
                    if (!p.key || !ParseVp6KeyFrame(h + lead, n - lead, adj >> 4, adj & 15,
                            &mVideo.width, &mVideo.height)) {
                        return;
                    }
                    mVideo.csd0.assign(1, h[1]);
                }
                mVideo.mime = kMimeVp6;
                p.offset = body + 2;
                p.size = size - 2;
                p.timeUs = timeMs * 1000;
                break;
            }
            default: {  // kCodecAvc
                if (n < 5) {
                    return;
                }
                const uint8_t packetType = h[1];
                // CompositionTime: signed 24-bit milliseconds from decode to
                // presentation time.
                int32_t cts = static_cast<int32_t>(Be24(h + 2));
                if (cts & 0x800000) {
                    cts -= 0x1000000;
                }
                if (packetType == 0) {
                    if (size > 4096 + 5) {
                        return;
                    }
                    std::vector<uint8_t> config(size - 5);
                    AvcConfig avc;
                    if (!ReadFully(mSource, body + 5, config.data(), config.size())
                            || !ParseAvcConfig(config.data(), config.size(), &avc)) {
                        return;
                    }
                    mVideo.csd0 = avc.sps;
                    mVideo.csd1 = avc.pps;
                    mVideo.width = avc.width;
                    mVideo.height = avc.height;
                    mVideo.nalLengthSize = avc.nalLengthSize;
                    mVideo.mime = kMimeAvc;
                    mVideo.transform = Transform::kAvcAnnexB;
                    return;
                }
                if (packetType != 1 || mVideo.width == 0 || size <= 5) {
                    return;
                }
                p.offset = body + 5;
                p.size = size - 5;
                p.timeUs = (timeMs + cts) * 1000;
                break;
            }
        }
        mVideo.maxInput = std::max<size_t>(mVideo.maxInput, p.size);
        if (mVideo.packets.size() < kMaxPackets) {
            mVideo.packets.push_back(p);
        }
    }

    void addAudio(uint64_t body, uint32_t size, int64_t timeMs) {
        uint8_t h[8];
        const size_t n = std::min<size_t>(size, sizeof(h));
        if (!ReadFully(mSource, body, h, n)) {
            return;
        }
        const uint8_t format = h[0] >> 4;
        if (format == kSoundMp3 || format == kSoundMp3Rate8k) {
            if (mAudio.mime.empty() && n >= 5) {
                Mp3Header mp3;
                if (ParseMp3Header(h + 1, n - 1, &mp3)) {
                    mAudio.mime = kMimeMp3;
                    mAudio.transform = Transform::kMp3Frames;
                    mAudio.sampleRate = mp3.sampleRate;
                    mAudio.channels = mp3.channels;
                    mAudio.bitrate = mp3.bitrate;
                }
            }
            if (mAudio.mime == kMimeMp3) {
                push(&mAudio, body + 1, size - 1, timeMs);
            }
        } else if (format == kSoundAac && n >= 2) {
            if (h[1] == 0) {
                // AudioSpecificConfig; the bound comes before the allocation.
                const size_t ascSize = size - 2;
                if (ascSize < 2 || ascSize > kMaxAscBytes) {
                    return;
                }
                std::vector<uint8_t> asc(ascSize);
                AacConfig cfg;
                if (ReadFully(mSource, body + 2, asc.data(), asc.size())
                        && ParseAacConfig(asc.data(), asc.size(), &cfg)) {
                    mAudio.mime = kMimeAac;
                    mAudio.csd0 = asc;
                    mAudio.sampleRate = cfg.sampleRate;
                    mAudio.channels = cfg.channels;
                }
            } else if (h[1] == 1 && mAudio.mime == kMimeAac) {
                push(&mAudio, body + 2, size - 2, timeMs);
            }
        }
    }

    void push(TrackInfo *t, uint64_t offset, uint32_t size, int64_t timeMs) {
        if (size == 0 || t->packets.size() >= kMaxPackets) {
            return;
        }
        t->maxInput = std::max<size_t>(t->maxInput, size);
        Packet p;
        p.offset = offset;
        p.size = size;
        p.timeUs = timeMs * 1000;
        p.key = true;
        t->packets.push_back(p);
    }

    bool finish(Container *out) {
        out->tracks.clear();
        if (!mVideo.packets.empty() && mVideo.width > 0 && !mVideo.mime.empty()) {
            if (mVideo.packets.size() > 1) {
                int64_t lo = mVideo.packets.front().timeUs;
                int64_t hi = lo;
                for (const Packet &p : mVideo.packets) {
                    lo = std::min(lo, p.timeUs);
                    hi = std::max(hi, p.timeUs);
                }
                if (hi > lo) {
                    mVideo.frameRate = static_cast<int32_t>(std::min<int64_t>(
                            (static_cast<int64_t>(mVideo.packets.size() - 1) * 1000000 + (hi - lo) / 2)
                            / (hi - lo), 240));
                }
            }
            // The first packet starts a decodable sequence.
            mVideo.packets.front().key = true;
            if (mVideo.transform == Transform::kAvcAnnexB) {
                mVideo.maxInput += mVideo.maxInput / 4;
            }
            out->tracks.push_back(mVideo);
        }
        if (!mAudio.packets.empty() && !mAudio.mime.empty()) {
            out->tracks.push_back(mAudio);
        }
        for (const TrackInfo &t : out->tracks) {
            for (const Packet &p : t.packets) {
                out->durationUs = std::max(out->durationUs, p.timeUs);
            }
        }
        for (TrackInfo &t : out->tracks) {
            t.durationUs = out->durationUs;
            t.maxInput = std::max<size_t>(t.maxInput, 4096);
        }
        return !out->tracks.empty();
    }

    ByteSource *mSource = nullptr;
    TrackInfo mVideo;
    TrackInfo mAudio;
    uint8_t mVideoCodec = 0;
};

}  // namespace

bool ParseFlv(ByteSource *source, Container *out) {
    FlvFile file;
    return file.parse(source, out);
}

}  // namespace a11
