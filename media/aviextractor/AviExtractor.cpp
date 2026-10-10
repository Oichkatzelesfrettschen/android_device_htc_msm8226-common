/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MediaExtractor plugin for AVI files carrying DivX, MPEG-4 Part 2, MP3,
 * AAC and PCM streams.
 */

#define LOG_TAG "AviExtractor"

#include <inttypes.h>
#include <string.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <media/MediaExtractorPluginApi.h>
#include <media/MediaExtractorPluginHelper.h>
#include <media/NdkMediaFormat.h>
#include <utils/Log.h>

namespace android {

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16)
            | ((uint32_t)(uint8_t)d << 24);
}

// The OMX component roles key on these MIME types: video/divx311 is MS-MPEG4
// version 3, video/divx4 is DivX 4, video/divx is DivX 5 and 6.
constexpr const char *kMimeDivx311 = "video/divx311";
constexpr const char *kMimeDivx4 = "video/divx4";
constexpr const char *kMimeDivx = "video/divx";
constexpr const char *kMimeMpeg4 = "video/mp4v-es";
constexpr const char *kMimeMp3 = "audio/mpeg";
constexpr const char *kMimeAac = "audio/mp4a-latm";
constexpr const char *kMimeRaw = "audio/raw";

constexpr size_t kMaxStreams = 16;
constexpr uint32_t kMaxChunk = 64u << 20;
constexpr uint32_t kMaxIndexEntries = 4u << 20;
constexpr uint32_t kMaxDimension = 4096;
constexpr uint32_t kIdx1KeyFrame = 0x10;

uint32_t Le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t Le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

struct Packet {
    uint64_t offset;
    uint32_t size;
    int64_t timeUs;
    bool key;
};

struct Stream {
    bool video = false;
    bool valid = false;
    std::string mime;
    std::vector<uint8_t> csd0;
    // strh
    uint32_t handler = 0;
    uint32_t scale = 1;
    uint32_t rate = 1;
    uint32_t sampleSize = 0;
    uint32_t suggested = 0;
    // strf
    int32_t width = 0;
    int32_t height = 0;
    int32_t channels = 0;
    int32_t sampleRate = 0;
    int32_t bitrate = 0;
    int32_t bitsPerSample = 0;
    uint32_t compression = 0;
    uint32_t formatTag = 0;
    // derived
    size_t maxChunk = 0;
    std::vector<Packet> packets;
    int32_t frameRate = 0;
};

bool DescribeVideo(Stream *s) {
    const uint32_t c = s->compression ? s->compression : s->handler;
    // Fourcc comparison is case-insensitive: encoders write both "DIVX" and
    // "divx".
    uint32_t upper = c & 0xDFDFDFDFu;
    switch (upper) {
        case FourCC('D', 'I', 'V', '3'):  // DivX 3 low motion
        case FourCC('D', 'I', 'V', '4'):  // DivX 3 fast motion
        case FourCC('D', 'I', 'V', '5'):
        case FourCC('D', 'I', 'V', '6'):
        case FourCC('M', 'P', '4', '3'):  // Microsoft MPEG-4 version 3
        case FourCC('M', 'P', 'G', '3'):
        case FourCC('C', 'O', 'L', '1'):
        case FourCC('C', 'O', 'L', '0'):
            s->mime = kMimeDivx311;
            break;
        case FourCC('D', 'I', 'V', 'X'):  // DivX 4
            s->mime = kMimeDivx4;
            break;
        case FourCC('D', 'X', '5', '0'):  // DivX 5 and 6
            s->mime = kMimeDivx;
            break;
        case FourCC('X', 'V', 'I', 'D'):
        case FourCC('M', 'P', '4', 'V'):
        case FourCC('F', 'M', 'P', '4'):
        case FourCC('M', '4', 'S', '2'):
        case FourCC('3', 'I', 'V', '2'):
            s->mime = kMimeMpeg4;
            break;
        default:
            return false;
    }
    return s->width > 0 && s->height > 0 && (uint32_t)s->width <= kMaxDimension
            && (uint32_t)s->height <= kMaxDimension;
}

bool DescribeAudio(Stream *s) {
    switch (s->formatTag) {
        case 0x0055:  // MPEG-1 Layer III
            s->mime = kMimeMp3;
            break;
        case 0x00FF:  // AAC; WAVEFORMATEX extra data is the AudioSpecificConfig
            if (s->csd0.empty()) {
                return false;
            }
            s->mime = kMimeAac;
            break;
        case 0x0001:  // PCM
            if (s->bitsPerSample != 16) {
                return false;
            }
            s->mime = kMimeRaw;
            break;
        default:
            return false;
    }
    return s->channels > 0 && s->channels <= 8 && s->sampleRate > 0;
}

class AviFile {
public:
    bool parse(DataSourceHelper *source) {
        mSource = source;
        uint8_t head[12];
        if (!readFully(0, head, sizeof(head)) || Le32(head) != FourCC('R', 'I', 'F', 'F')
                || Le32(head + 8) != FourCC('A', 'V', 'I', ' ')) {
            return false;
        }
        const uint64_t riffEnd = std::min<uint64_t>((uint64_t)Le32(head + 4) + 8,
                fileSize());
        uint64_t pos = 12;
        bool haveMovi = false;
        uint64_t moviStart = 0;
        uint64_t moviEnd = 0;
        std::vector<uint8_t> idx1;
        while (pos + 8 <= riffEnd) {
            uint8_t ch[12];
            if (!readFully(pos, ch, 8)) {
                break;
            }
            const uint32_t id = Le32(ch);
            const uint32_t size = Le32(ch + 4);
            const uint64_t body = pos + 8;
            if (id == FourCC('L', 'I', 'S', 'T')) {
                if (!readFully(body, ch + 8, 4)) {
                    break;
                }
                const uint32_t type = Le32(ch + 8);
                if (type == FourCC('h', 'd', 'r', 'l')) {
                    if (!parseHeaderList(body + 4, std::min<uint64_t>(body + size, riffEnd))) {
                        return false;
                    }
                } else if (type == FourCC('m', 'o', 'v', 'i') && !haveMovi) {
                    haveMovi = true;
                    moviStart = body + 4;
                    moviEnd = std::min<uint64_t>(body + size, riffEnd);
                }
            } else if (id == FourCC('i', 'd', 'x', '1') && size >= 16 && size <= (kMaxIndexEntries * 16u)
                    && body + size <= riffEnd) {
                idx1.resize(size);
                if (!readFully(body, idx1.data(), size)) {
                    idx1.clear();
                }
            }
            pos = body + size + (size & 1);
        }
        if (!haveMovi || mStreams.empty()) {
            return false;
        }
        if (idx1.empty() || !buildFromIndex(idx1, moviStart, moviEnd)) {
            scanMovi(moviStart, moviEnd);
        }
        return finish();
    }

    std::vector<Stream> &streams() { return mStreams; }
    int64_t durationUs() const { return mDurationUs; }

private:
    uint64_t fileSize() {
        off64_t size = 0;
        if (mSource->getSize(&size) != OK || size <= 0) {
            return UINT64_MAX;
        }
        return (uint64_t)size;
    }

    bool readFully(uint64_t offset, void *data, size_t size) {
        return mSource->readAt((off64_t)offset, data, size) == (ssize_t)size;
    }

    bool parseHeaderList(uint64_t pos, uint64_t end) {
        uint32_t microsPerFrame = 0;
        while (pos + 8 <= end) {
            uint8_t ch[12];
            if (!readFully(pos, ch, 8)) {
                return false;
            }
            const uint32_t id = Le32(ch);
            const uint32_t size = Le32(ch + 4);
            const uint64_t body = pos + 8;
            if (body + size > end) {
                return false;
            }
            if (id == FourCC('a', 'v', 'i', 'h') && size >= 4) {
                uint8_t avih[4];
                if (readFully(body, avih, 4)) {
                    microsPerFrame = Le32(avih);
                }
            } else if (id == FourCC('L', 'I', 'S', 'T') && size >= 4) {
                uint8_t type[4];
                if (!readFully(body, type, 4)) {
                    return false;
                }
                if (Le32(type) == FourCC('s', 't', 'r', 'l')) {
                    if (mStreams.size() >= kMaxStreams) {
                        return false;
                    }
                    Stream s;
                    parseStreamList(body + 4, body + size, &s);
                    if (s.video && s.frameRate == 0 && microsPerFrame > 0) {
                        s.frameRate = (int32_t)((1000000ull + microsPerFrame / 2) / microsPerFrame);
                    }
                    mStreams.push_back(std::move(s));
                }
            }
            pos = body + size + (size & 1);
        }
        return !mStreams.empty();
    }

    void parseStreamList(uint64_t pos, uint64_t end, Stream *s) {
        uint32_t type = 0;
        while (pos + 8 <= end) {
            uint8_t ch[8];
            if (!readFully(pos, ch, 8)) {
                return;
            }
            const uint32_t id = Le32(ch);
            const uint32_t size = Le32(ch + 4);
            const uint64_t body = pos + 8;
            if (body + size > end) {
                return;
            }
            if (id == FourCC('s', 't', 'r', 'h') && size >= 48) {
                uint8_t h[48];
                if (!readFully(body, h, sizeof(h))) {
                    return;
                }
                type = Le32(h);
                s->handler = Le32(h + 4);
                s->scale = std::max<uint32_t>(Le32(h + 20), 1);
                s->rate = std::max<uint32_t>(Le32(h + 24), 1);
                s->suggested = std::min<uint32_t>(Le32(h + 36), kMaxChunk);
                s->sampleSize = Le32(h + 44);
                s->video = type == FourCC('v', 'i', 'd', 's');
                if (s->video && s->rate / s->scale > 0) {
                    s->frameRate = (int32_t)std::min<uint64_t>(
                            ((uint64_t)s->rate + s->scale / 2) / s->scale, 240);
                }
            } else if (id == FourCC('s', 't', 'r', 'f')) {
                std::vector<uint8_t> f(std::min<uint32_t>(size, 1u << 20));
                if (!f.empty() && !readFully(body, f.data(), f.size())) {
                    return;
                }
                if (type == FourCC('v', 'i', 'd', 's') && f.size() >= 40) {
                    s->width = (int32_t)Le32(f.data() + 4);
                    s->height = std::abs((int32_t)Le32(f.data() + 8));
                    s->compression = Le32(f.data() + 16);
                    uint32_t headerSize = std::max<uint32_t>(Le32(f.data()), 40);
                    if (headerSize < f.size()) {
                        s->csd0.assign(f.begin() + headerSize, f.end());
                    }
                    s->valid = DescribeVideo(s);
                    // Codec data of an MPEG-4 stream is in the bitstream.
                    s->csd0.clear();
                } else if (type == FourCC('a', 'u', 'd', 's') && f.size() >= 16) {
                    s->formatTag = Le16(f.data());
                    s->channels = Le16(f.data() + 2);
                    s->sampleRate = (int32_t)Le32(f.data() + 4);
                    s->bitrate = (int32_t)std::min<uint32_t>(Le32(f.data() + 8) * 8ull, INT32_MAX);
                    s->bitsPerSample = f.size() >= 16 ? Le16(f.data() + 14) : 0;
                    if (f.size() >= 18) {
                        const uint32_t cb = Le16(f.data() + 16);
                        if (cb > 0 && 18 + cb <= f.size()) {
                            s->csd0.assign(f.begin() + 18, f.begin() + 18 + cb);
                        }
                    }
                    s->valid = DescribeAudio(s);
                }
            }
            pos = body + size + (size & 1);
        }
    }

    // idx1 offsets are relative to the 'movi' list type field in most files
    // and to the start of the file in the rest; the first chunk header names
    // the base.
    bool buildFromIndex(const std::vector<uint8_t> &idx1, uint64_t moviStart, uint64_t moviEnd) {
        const size_t count = idx1.size() / 16;
        if (count == 0) {
            return false;
        }
        uint64_t base = moviStart - 4;
        {
            uint8_t ch[4];
            const uint64_t first = (uint64_t)Le32(idx1.data() + 8);
            if (!(readFully(base + first, ch, 4) && memcmp(ch, idx1.data(), 4) == 0)) {
                base = 0;
                if (!(readFully(first, ch, 4) && memcmp(ch, idx1.data(), 4) == 0)) {
                    return false;
                }
            }
        }
        std::vector<uint64_t> bytes(mStreams.size(), 0);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *e = idx1.data() + i * 16;
            const char d0 = (char)e[0];
            const char d1 = (char)e[1];
            if (d0 < '0' || d0 > '9' || d1 < '0' || d1 > '9') {
                continue;  // 'rec ' lists and junk
            }
            const size_t n = (size_t)((d0 - '0') * 10 + (d1 - '0'));
            if (n >= mStreams.size() || !mStreams[n].valid) {
                continue;
            }
            Stream &s = mStreams[n];
            const uint32_t flags = Le32(e + 4);
            const uint64_t offset = base + Le32(e + 8) + 8;
            const uint32_t size = Le32(e + 12);
            if (size == 0 || size > kMaxChunk || offset + size > moviEnd) {
                continue;
            }
            Packet p;
            p.offset = offset;
            p.size = size;
            p.key = s.video ? (flags & kIdx1KeyFrame) != 0 : true;
            const uint64_t index = s.packets.size();
            if (s.sampleSize > 0 && !s.video) {
                p.timeUs = (int64_t)((bytes[n] / s.sampleSize) * s.scale * 1000000ull / s.rate);
            } else {
                p.timeUs = (int64_t)(index * s.scale * 1000000ull / s.rate);
            }
            bytes[n] += size;
            s.maxChunk = std::max<size_t>(s.maxChunk, size);
            s.packets.push_back(p);
        }
        return true;
    }

    // Without a usable idx1 the chunk headers of the movi list give the
    // packets; only the first video chunk is known to be a sync frame.
    void scanMovi(uint64_t pos, uint64_t end) {
        std::vector<uint64_t> bytes(mStreams.size(), 0);
        uint32_t guard = 0;
        while (pos + 8 <= end && guard++ < kMaxIndexEntries) {
            uint8_t ch[12];
            if (!readFully(pos, ch, 8)) {
                return;
            }
            const uint32_t id = Le32(ch);
            const uint32_t size = Le32(ch + 4);
            const uint64_t body = pos + 8;
            if (id == FourCC('L', 'I', 'S', 'T')) {
                pos = body + 4;  // descend into a 'rec ' list
                continue;
            }
            if (body + size > end) {
                return;
            }
            const char d0 = (char)ch[0];
            const char d1 = (char)ch[1];
            if (d0 >= '0' && d0 <= '9' && d1 >= '0' && d1 <= '9' && size > 0 && size <= kMaxChunk) {
                const size_t n = (size_t)((d0 - '0') * 10 + (d1 - '0'));
                if (n < mStreams.size() && mStreams[n].valid) {
                    Stream &s = mStreams[n];
                    Packet p;
                    p.offset = body;
                    p.size = size;
                    p.key = !s.video;
                    const uint64_t index = s.packets.size();
                    if (s.sampleSize > 0 && !s.video) {
                        p.timeUs = (int64_t)((bytes[n] / s.sampleSize) * s.scale * 1000000ull / s.rate);
                    } else {
                        p.timeUs = (int64_t)(index * s.scale * 1000000ull / s.rate);
                    }
                    bytes[n] += size;
                    s.maxChunk = std::max<size_t>(s.maxChunk, size);
                    s.packets.push_back(p);
                }
            }
            pos = body + size + (size & 1);
        }
    }

    bool finish() {
        bool any = false;
        for (Stream &s : mStreams) {
            if (!s.valid || s.packets.empty()) {
                s.valid = false;
                continue;
            }
            any = true;
            // The first video chunk always starts a decodable sequence.
            if (s.video) {
                s.packets.front().key = true;
            }
            const Packet &last = s.packets.back();
            int64_t end = last.timeUs;
            if (s.video) {
                end += (int64_t)(s.scale * 1000000ull / s.rate);
            }
            mDurationUs = std::max(mDurationUs, end);
        }
        return any;
    }

    DataSourceHelper *mSource = nullptr;
    std::vector<Stream> mStreams;
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
        if (s.mime == kMimeRaw) {
            AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_PCM_ENCODING, 2 /* ENCODING_PCM_16BIT */);
        }
    }
    if (s.bitrate > 0) {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_BIT_RATE, s.bitrate);
    }
    if (durationUs > 0) {
        AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, durationUs);
    }
    const size_t maxInput = std::max<size_t>(std::max<size_t>(s.maxChunk, s.suggested), 4096);
    AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, (int32_t)maxInput);
}

class AviTrack : public MediaTrackHelper {
public:
    AviTrack(DataSourceHelper *source, const Stream *stream, int64_t durationUs)
        : mSource(source), mStream(stream), mDurationUs(durationUs) {}

    media_status_t start() override {
        const size_t maxInput = std::max<size_t>(mStream->maxChunk, 4096);
        if (!mBufferGroup->init(4 /* buffers */, maxInput, 16 /* growth limit */)) {
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

class AviExtractor : public MediaExtractorPluginHelper {
public:
    explicit AviExtractor(DataSourceHelper *source) : mSource(source) {
        if (mFile.parse(mSource)) {
            for (size_t i = 0; i < mFile.streams().size(); ++i) {
                if (mFile.streams()[i].valid) {
                    mTracks.push_back(i);
                }
            }
        }
    }

    ~AviExtractor() override { delete mSource; }

    size_t countTracks() override { return mTracks.size(); }

    MediaTrackHelper *getTrack(size_t index) override {
        if (index >= mTracks.size()) {
            return nullptr;
        }
        return new AviTrack(mSource, &mFile.streams()[mTracks[index]], mFile.durationUs());
    }

    media_status_t getTrackMetaData(AMediaFormat *meta, size_t index, uint32_t) override {
        if (index >= mTracks.size()) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        FillFormat(mFile.streams()[mTracks[index]], mFile.durationUs(), meta);
        return AMEDIA_OK;
    }

    media_status_t getMetaData(AMediaFormat *meta) override {
        AMediaFormat_clear(meta);
        AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, "video/avi");
        if (mFile.durationUs() > 0) {
            AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, mFile.durationUs());
        }
        return AMEDIA_OK;
    }

    const char *name() override { return "AviExtractor"; }

private:
    DataSourceHelper *mSource;
    AviFile mFile;
    std::vector<size_t> mTracks;
};

bool SniffAvi(DataSourceHelper *source, float *confidence) {
    uint8_t header[12];
    if (source->readAt(0, header, sizeof(header)) != (ssize_t)sizeof(header)
            || Le32(header) != FourCC('R', 'I', 'F', 'F')
            || Le32(header + 8) != FourCC('A', 'V', 'I', ' ')) {
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
        1,
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
                        return wrap(new AviExtractor(new DataSourceHelper(source)));
                    };
                },
                kExtensions,
            },
        },
    };
}

}  // extern "C"

}  // namespace android
