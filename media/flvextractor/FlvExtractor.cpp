/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MediaExtractor plugin for FLV files carrying Sorenson Spark video and MP3
 * or AAC audio.
 */

#define LOG_TAG "FlvExtractor"

#include <inttypes.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <media/MediaExtractorPluginApi.h>
#include <media/MediaExtractorPluginHelper.h>
#include <media/NdkMediaFormat.h>
#include <utils/Log.h>

namespace android {

namespace {

// video/x-flv1 names the Sorenson Spark (FLV1) elementary stream; the
// container type is video/x-flv.
constexpr const char *kMimeSpark = "video/x-flv1";
constexpr const char *kMimeMp3 = "audio/mpeg";
constexpr const char *kMimeAac = "audio/mp4a-latm";

constexpr uint8_t kTagAudio = 8;
constexpr uint8_t kTagVideo = 9;
constexpr uint32_t kMaxTag = 32u << 20;
constexpr size_t kMaxTags = 4u << 20;

constexpr uint8_t kCodecSpark = 2;
constexpr uint8_t kSoundMp3 = 2;
constexpr uint8_t kSoundAac = 10;
constexpr uint8_t kFrameKey = 1;
constexpr uint8_t kFrameInfo = 5;  // video info/command frame: no picture

uint32_t Be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

uint32_t Be32(const uint8_t *p) { return (Be24(p) << 8) | p[3]; }

const int32_t kMp3Rates[3][4] = {
    {44100, 48000, 32000, 0},   // MPEG-1
    {22050, 24000, 16000, 0},   // MPEG-2
    {11025, 12000, 8000, 0},    // MPEG-2.5
};

const int32_t kAacRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                               22050, 16000, 12000, 11025, 8000, 7350};

struct Packet {
    uint64_t offset;
    uint32_t size;
    int64_t timeUs;
    bool key;
};

struct Stream {
    bool video = false;
    std::string mime;
    std::vector<uint8_t> csd0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t channels = 0;
    int32_t sampleRate = 0;
    int32_t frameRate = 0;
    size_t maxPacket = 0;
    std::vector<Packet> packets;
};

// Bit reader for the Spark picture header, an H.263 variant.
class BitReader {
public:
    BitReader(const uint8_t *data, size_t size) : mData(data), mSize(size) {}
    bool get(unsigned bits, uint32_t *value) {
        uint32_t v = 0;
        for (unsigned i = 0; i < bits; ++i) {
            if (mPos >= mSize * 8) {
                return false;
            }
            v = (v << 1) | ((mData[mPos >> 3] >> (7 - (mPos & 7))) & 1);
            ++mPos;
        }
        *value = v;
        return true;
    }

private:
    const uint8_t *mData;
    size_t mSize;
    size_t mPos = 0;
};

// Spark picture header: 17-bit start code (1), 5-bit version, 8-bit temporal
// reference, 3-bit size code, then explicit 8- or 16-bit width and height or
// one of five fixed sizes.
bool ParseSparkSize(const uint8_t *data, size_t size, int32_t *width, int32_t *height) {
    BitReader br(data, size);
    uint32_t start, version, tref, code, w = 0, h = 0;
    if (!br.get(17, &start) || start != 1 || !br.get(5, &version) || version > 1
            || !br.get(8, &tref) || !br.get(3, &code)) {
        return false;
    }
    switch (code) {
        case 0:
            if (!br.get(8, &w) || !br.get(8, &h)) return false;
            break;
        case 1:
            if (!br.get(16, &w) || !br.get(16, &h)) return false;
            break;
        case 2: w = 352; h = 288; break;
        case 3: w = 176; h = 144; break;
        case 4: w = 128; h = 96; break;
        case 5: w = 320; h = 240; break;
        case 6: w = 160; h = 120; break;
        default: return false;
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        return false;
    }
    *width = (int32_t)w;
    *height = (int32_t)h;
    return true;
}

// MPEG audio frame header: version, layer, sample rate and channel mode.
bool ParseMp3Header(const uint8_t *p, size_t size, int32_t *rate, int32_t *channels) {
    if (size < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return false;
    }
    const unsigned version = (p[1] >> 3) & 3;  // 0: 2.5, 2: 2, 3: 1
    const unsigned layer = (p[1] >> 1) & 3;    // 1: Layer III
    const unsigned rateIdx = (p[2] >> 2) & 3;
    if (version == 1 || layer != 1 || rateIdx == 3) {
        return false;
    }
    const int row = version == 3 ? 0 : (version == 2 ? 1 : 2);
    *rate = kMp3Rates[row][rateIdx];
    *channels = ((p[3] >> 6) & 3) == 3 ? 1 : 2;
    return *rate > 0;
}

class FlvFile {
public:
    bool parse(DataSourceHelper *source) {
        mSource = source;
        uint8_t head[13];
        if (!readFully(0, head, sizeof(head)) || memcmp(head, "FLV", 3) != 0 || head[3] != 1) {
            return false;
        }
        const uint32_t dataOffset = Be32(head + 5);
        if (dataOffset < 9) {
            return false;
        }
        off64_t total = 0;
        if (mSource->getSize(&total) != OK || total <= 0) {
            return false;
        }
        mVideo.video = true;
        uint64_t pos = (uint64_t)dataOffset + 4;  // skip PreviousTagSize0
        size_t tags = 0;
        while (pos + 11 <= (uint64_t)total && tags++ < kMaxTags) {
            uint8_t th[16];
            if (!readFully(pos, th, 11)) {
                break;
            }
            const uint8_t type = th[0] & 0x1F;
            const uint32_t size = Be24(th + 1);
            const int64_t timeMs = (int64_t)(Be24(th + 4) | ((uint32_t)th[7] << 24));
            const uint64_t body = pos + 11;
            if (size > kMaxTag || body + size > (uint64_t)total) {
                break;
            }
            if (type == kTagVideo && size >= 2) {
                addVideo(body, size, timeMs);
            } else if (type == kTagAudio && size >= 2) {
                addAudio(body, size, timeMs);
            }
            pos = body + size + 4;  // PreviousTagSize
        }
        return finish();
    }

    std::vector<Stream *> &tracks() { return mTracks; }
    int64_t durationUs() const { return mDurationUs; }

private:
    bool readFully(uint64_t offset, void *data, size_t size) {
        return mSource->readAt((off64_t)offset, data, size) == (ssize_t)size;
    }

    void addVideo(uint64_t body, uint32_t size, int64_t timeMs) {
        uint8_t h[1];
        if (!readFully(body, h, 1)) {
            return;
        }
        const uint8_t codec = h[0] & 0x0F;
        const uint8_t frameType = h[0] >> 4;
        if (codec != kCodecSpark || frameType == kFrameInfo) {
            return;
        }
        const uint32_t payload = size - 1;
        if (mVideo.width == 0) {
            uint8_t peek[16];
            const size_t n = std::min<size_t>(payload, sizeof(peek));
            if (!readFully(body + 1, peek, n)
                    || !ParseSparkSize(peek, n, &mVideo.width, &mVideo.height)) {
                return;  // wait for a picture that carries a header
            }
        }
        Packet p{body + 1, payload, timeMs * 1000, frameType == kFrameKey};
        mVideo.maxPacket = std::max<size_t>(mVideo.maxPacket, payload);
        mVideo.packets.push_back(p);
    }

    void addAudio(uint64_t body, uint32_t size, int64_t timeMs) {
        uint8_t h[6];
        const size_t n = std::min<size_t>(size, sizeof(h));
        if (!readFully(body, h, n)) {
            return;
        }
        const uint8_t format = h[0] >> 4;
        if (format == kSoundMp3) {
            if (mAudio.mime.empty() && n >= 5) {
                int32_t rate = 0, channels = 0;
                if (ParseMp3Header(h + 1, n - 1, &rate, &channels)) {
                    mAudio.mime = kMimeMp3;
                    mAudio.sampleRate = rate;
                    mAudio.channels = channels;
                }
            }
            if (mAudio.mime == kMimeMp3) {
                push(&mAudio, body + 1, size - 1, timeMs);
            }
        } else if (format == kSoundAac && n >= 2) {
            if (h[1] == 0) {
                // AudioSpecificConfig
                std::vector<uint8_t> asc(size - 2);
                if (asc.size() >= 2 && asc.size() <= 64 && readFully(body + 2, asc.data(), asc.size())) {
                    const unsigned rateIdx = ((asc[0] & 7) << 1) | (asc[1] >> 7);
                    const int32_t channels = (asc[1] >> 3) & 15;
                    if (rateIdx < 13 && channels >= 1 && channels <= 7) {
                        mAudio.mime = kMimeAac;
                        mAudio.csd0 = asc;
                        mAudio.sampleRate = kAacRates[rateIdx];
                        mAudio.channels = channels;
                    }
                }
            } else if (mAudio.mime == kMimeAac) {
                push(&mAudio, body + 2, size - 2, timeMs);
            }
        }
    }

    void push(Stream *s, uint64_t offset, uint32_t size, int64_t timeMs) {
        if (size == 0) {
            return;
        }
        s->maxPacket = std::max<size_t>(s->maxPacket, size);
        s->packets.push_back(Packet{offset, size, timeMs * 1000, true});
    }

    bool finish() {
        if (!mVideo.packets.empty() && mVideo.width > 0) {
            mVideo.mime = kMimeSpark;
            if (mVideo.packets.size() > 1) {
                const int64_t span = mVideo.packets.back().timeUs - mVideo.packets.front().timeUs;
                if (span > 0) {
                    mVideo.frameRate = (int32_t)std::min<int64_t>(
                            ((int64_t)(mVideo.packets.size() - 1) * 1000000 + span / 2) / span, 240);
                }
            }
            mVideo.packets.front().key = true;
            mTracks.push_back(&mVideo);
        }
        if (!mAudio.packets.empty() && !mAudio.mime.empty()) {
            mTracks.push_back(&mAudio);
        }
        for (const Stream *s : mTracks) {
            mDurationUs = std::max(mDurationUs, s->packets.back().timeUs);
        }
        return !mTracks.empty();
    }

    DataSourceHelper *mSource = nullptr;
    Stream mVideo;
    Stream mAudio;
    std::vector<Stream *> mTracks;
    int64_t mDurationUs = 0;
};

void FillFormat(const Stream &s, int64_t durationUs, AMediaFormat *meta) {
    AMediaFormat_clear(meta);
    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, s.mime.c_str());
    if (!s.csd0.empty()) {
        AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_CSD_0, s.csd0.data(), s.csd0.size());
    }
    if (s.video) {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_WIDTH, s.width);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_HEIGHT, s.height);
        if (s.frameRate > 0) {
            AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_FRAME_RATE, s.frameRate);
        }
    } else {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_CHANNEL_COUNT, s.channels);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_SAMPLE_RATE, s.sampleRate);
    }
    if (durationUs > 0) {
        AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, durationUs);
    }
    AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
            (int32_t)std::max<size_t>(s.maxPacket, 4096));
}

class FlvTrack : public MediaTrackHelper {
public:
    FlvTrack(DataSourceHelper *source, const Stream *stream, int64_t durationUs)
        : mSource(source), mStream(stream), mDurationUs(durationUs) {}

    media_status_t start() override {
        if (!mBufferGroup->init(4 /* buffers */, std::max<size_t>(mStream->maxPacket, 4096),
                16 /* growth limit */)) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        mNext = 0;
        return AMEDIA_OK;
    }

    media_status_t stop() override { return AMEDIA_OK; }

    media_status_t getFormat(AMediaFormat *meta) override {
        FillFormat(*mStream, mDurationUs, meta);
        return AMEDIA_OK;
    }

    media_status_t read(MediaBufferHelper **out, const ReadOptions *options) override {
        *out = nullptr;
        int64_t seekTimeUs;
        ReadOptions::SeekMode mode;
        int64_t targetTimeUs = -1;
        const std::vector<Packet> &packets = mStream->packets;
        if (options != nullptr && options->getSeekTo(&seekTimeUs, &mode)) {
            size_t i = 0;
            while (i + 1 < packets.size() && packets[i + 1].timeUs <= seekTimeUs) {
                ++i;
            }
            if (mode == ReadOptions::SEEK_NEXT_SYNC && packets[i].timeUs < seekTimeUs
                    && i + 1 < packets.size()) {
                ++i;
                while (i + 1 < packets.size() && !packets[i].key) {
                    ++i;
                }
            } else {
                while (i > 0 && !packets[i].key) {
                    --i;
                }
            }
            mNext = i;
            if (mode == ReadOptions::SEEK_CLOSEST) {
                targetTimeUs = seekTimeUs;
            }
        }
        if (mNext >= packets.size()) {
            return AMEDIA_ERROR_END_OF_STREAM;
        }
        const Packet &p = packets[mNext];
        MediaBufferHelper *buffer = nullptr;
        status_t err = mBufferGroup->acquire_buffer(&buffer, false /* nonBlocking */, p.size);
        if (err != OK || buffer == nullptr) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        if (buffer->size() < p.size) {
            buffer->release();
            return AMEDIA_ERROR_MALFORMED;
        }
        if (mSource->readAt((off64_t)p.offset, buffer->data(), p.size) != (ssize_t)p.size) {
            buffer->release();
            return AMEDIA_ERROR_IO;
        }
        ++mNext;
        buffer->set_range(0, p.size);
        AMediaFormat *meta = buffer->meta_data();
        AMediaFormat_clear(meta);
        AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_TIME_US, p.timeUs);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_IS_SYNC_FRAME, p.key ? 1 : 0);
        if (targetTimeUs >= 0) {
            AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_TARGET_TIME, targetTimeUs);
        }
        *out = buffer;
        return AMEDIA_OK;
    }

private:
    DataSourceHelper *mSource;
    const Stream *mStream;
    int64_t mDurationUs;
    size_t mNext = 0;
};

class FlvExtractor : public MediaExtractorPluginHelper {
public:
    explicit FlvExtractor(DataSourceHelper *source) : mSource(source) {
        mValid = mFile.parse(mSource);
    }

    ~FlvExtractor() override { delete mSource; }

    size_t countTracks() override { return mValid ? mFile.tracks().size() : 0; }

    MediaTrackHelper *getTrack(size_t index) override {
        if (index >= countTracks()) {
            return nullptr;
        }
        return new FlvTrack(mSource, mFile.tracks()[index], mFile.durationUs());
    }

    media_status_t getTrackMetaData(AMediaFormat *meta, size_t index, uint32_t) override {
        if (index >= countTracks()) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        FillFormat(*mFile.tracks()[index], mFile.durationUs(), meta);
        return AMEDIA_OK;
    }

    media_status_t getMetaData(AMediaFormat *meta) override {
        AMediaFormat_clear(meta);
        AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, "video/x-flv");
        if (mFile.durationUs() > 0) {
            AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, mFile.durationUs());
        }
        return AMEDIA_OK;
    }

    const char *name() override { return "FlvExtractor"; }

private:
    DataSourceHelper *mSource;
    FlvFile mFile;
    bool mValid = false;
};

bool SniffFlv(DataSourceHelper *source, float *confidence) {
    uint8_t header[9];
    if (source->readAt(0, header, sizeof(header)) != (ssize_t)sizeof(header)
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
        1,
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
                        return wrap(new FlvExtractor(new DataSourceHelper(source)));
                    };
                },
                kExtensions,
            },
        },
    };
}

}  // extern "C"

}  // namespace android
