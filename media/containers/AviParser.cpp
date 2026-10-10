/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * AVI (RIFF) demuxer tables: avih/strh/strf stream descriptions, idx1 and
 * OpenDML (AVIX segments, indx/ix## indexes) packet tables, and a movi scan
 * when no index exists.
 */

#include <string.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "Codecs.h"
#include "Tracks.h"

namespace a11 {

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a))
            | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
            | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
            | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

// MIME types the OMX component roles key on: video/divx311 is MS-MPEG4 version
// 3, video/divx4 is DivX 4, video/divx is DivX 5 and 6.
constexpr const char *kMimeDivx311 = "video/divx311";
constexpr const char *kMimeDivx4 = "video/divx4";
constexpr const char *kMimeDivx = "video/divx";
constexpr const char *kMimeMpeg4 = "video/mp4v-es";
constexpr const char *kMimeAvc = "video/avc";
constexpr const char *kMimeMp3 = "audio/mpeg";
constexpr const char *kMimeAac = "audio/mp4a-latm";
constexpr const char *kMimeRaw = "audio/raw";

constexpr size_t kMaxStreams = 16;
constexpr uint32_t kMaxDimension = 4096;
constexpr uint32_t kIdx1KeyFrame = 0x10;
constexpr size_t kMaxCodecData = 4096;
constexpr int32_t kEncodingPcm16 = 2;

uint32_t Le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
            | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t Le64(const uint8_t *p) {
    return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32);
}

uint16_t Le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Case folds the letters 'a'..'z' of a fourcc; digits keep their value.
constexpr uint32_t FoldUpper(uint32_t c) {
    uint32_t folded = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t b = (c >> shift) & 0xFF;
        if (b >= 'a' && b <= 'z') {
            b -= 'a' - 'A';
        }
        folded |= b << shift;
    }
    return folded;
}

struct SuperEntry {
    uint64_t offset;
    uint32_t size;
};

struct Stream {
    TrackInfo info;
    bool valid = false;
    uint32_t handler = 0;
    uint32_t scale = 1;
    uint32_t rate = 1;
    uint32_t start = 0;
    uint32_t sampleSize = 0;
    uint32_t suggested = 0;
    uint32_t formatTag = 0;
    uint32_t compression = 0;
    int32_t bitsPerSample = 0;
    std::vector<uint8_t> extra;
    std::vector<SuperEntry> superIndex;
    // Chunk ordinal and byte count for timestamping.
    uint64_t chunks = 0;
    uint64_t bytes = 0;
    bool timeOverflow = false;
};

bool DescribeVideo(Stream *s) {
    const uint32_t c = s->compression ? s->compression : s->handler;
    switch (FoldUpper(c)) {
        case FourCC('D', 'I', 'V', '3'):  // DivX 3 low motion
        case FourCC('D', 'I', 'V', '4'):  // DivX 3 fast motion
        case FourCC('D', 'I', 'V', '5'):
        case FourCC('D', 'I', 'V', '6'):
        case FourCC('M', 'P', '4', '3'):  // Microsoft MPEG-4 version 3
        case FourCC('M', 'P', 'G', '3'):
        case FourCC('C', 'O', 'L', '1'):
        case FourCC('C', 'O', 'L', '0'):
            s->info.mime = kMimeDivx311;
            break;
        case FourCC('D', 'I', 'V', 'X'):  // DivX 4
            s->info.mime = kMimeDivx4;
            break;
        case FourCC('D', 'X', '5', '0'):  // DivX 5 and 6
            s->info.mime = kMimeDivx;
            break;
        case FourCC('X', 'V', 'I', 'D'):
        case FourCC('M', 'P', '4', 'V'):
        case FourCC('F', 'M', 'P', '4'):
        case FourCC('M', '4', 'S', '2'):
        case FourCC('3', 'I', 'V', '2'):
            s->info.mime = kMimeMpeg4;
            // The visual object sequence and VOL header may live only in the
            // strf extension.
            if (!s->extra.empty() && s->extra.size() <= kMaxCodecData) {
                s->info.csd0 = s->extra;
            }
            break;
        case FourCC('A', 'V', 'C', '1'): {
            s->info.mime = kMimeAvc;
            // An AVC1 stream may carry an AVCDecoderConfigurationRecord in the
            // strf extension and length-prefixed NAL units in its chunks;
            // without one it is Annex B like H264.
            AvcConfig avc;
            if (!s->extra.empty() && s->extra[0] == 1
                    && ParseAvcConfig(s->extra.data(), s->extra.size(), &avc)) {
                s->info.csd0 = avc.sps;
                s->info.csd1 = avc.pps;
                s->info.nalLengthSize = avc.nalLengthSize;
                s->info.transform = Transform::kAvcAnnexB;
            }
            break;
        }
        case FourCC('H', '2', '6', '4'):
        case FourCC('X', '2', '6', '4'):
            s->info.mime = kMimeAvc;  // Annex B in AVI; parameter sets in band
            break;
        default:
            return false;
    }
    return s->info.width > 0 && s->info.height > 0
            && static_cast<uint32_t>(s->info.width) <= kMaxDimension
            && static_cast<uint32_t>(s->info.height) <= kMaxDimension;
}

bool DescribeAudio(Stream *s) {
    switch (s->formatTag) {
        case 0x0055:  // MPEG-1 Layer III
            s->info.mime = kMimeMp3;
            s->info.transform = Transform::kMp3Frames;
            break;
        case 0x00FF: {  // AAC; WAVEFORMATEX extra data is the AudioSpecificConfig
            AacConfig cfg;
            if (s->extra.empty() || s->extra.size() > kMaxCodecData
                    || !ParseAacConfig(s->extra.data(), s->extra.size(), &cfg)) {
                return false;
            }
            s->info.mime = kMimeAac;
            s->info.csd0 = s->extra;
            break;
        }
        case 0x0001:  // PCM
            if (s->bitsPerSample != 16) {
                return false;
            }
            s->info.mime = kMimeRaw;
            s->info.pcmEncoding = kEncodingPcm16;
            break;
        default:
            return false;
    }
    return s->info.channels > 0 && s->info.channels <= 8 && s->info.sampleRate > 0;
}

class AviFile {
public:
    bool parse(ByteSource *source) {
        mSource = source;
        const int64_t total = source->size();
        mFileSize = total > 0 ? static_cast<uint64_t>(total) : UINT64_MAX;
        std::vector<Segment> segments;
        uint64_t pos = 0;
        bool haveHeader = false;
        std::vector<uint8_t> idx1;
        while (pos + 12 <= mFileSize) {
            uint8_t head[12];
            if (!ReadFully(mSource, pos, head, sizeof(head)) || Le32(head) != FourCC('R', 'I', 'F', 'F')) {
                break;
            }
            const uint32_t form = Le32(head + 8);
            if ((!haveHeader && form != FourCC('A', 'V', 'I', ' '))
                    || (haveHeader && form != FourCC('A', 'V', 'I', 'X'))) {
                break;
            }
            const uint64_t end = std::min<uint64_t>(pos + 8 + static_cast<uint64_t>(Le32(head + 4)), mFileSize);
            uint64_t child = pos + 12;
            while (child + 8 <= end) {
                uint8_t ch[12];
                if (!ReadFully(mSource, child, ch, 8)) {
                    break;
                }
                const uint32_t id = Le32(ch);
                const uint32_t size = Le32(ch + 4);
                const uint64_t body = child + 8;
                if (id == FourCC('L', 'I', 'S', 'T') && size >= 4 && ReadFully(mSource, body, ch + 8, 4)) {
                    const uint32_t type = Le32(ch + 8);
                    if (type == FourCC('h', 'd', 'r', 'l') && !haveHeader) {
                        haveHeader = true;
                        if (!parseHeaderList(body + 4, std::min<uint64_t>(body + size, end))) {
                            return false;
                        }
                    } else if (type == FourCC('m', 'o', 'v', 'i')) {
                        segments.push_back({body + 4, std::min<uint64_t>(body + size, end)});
                    }
                } else if (id == FourCC('i', 'd', 'x', '1') && idx1.empty() && size >= 16
                        && size <= kMaxPackets * 16ull && body + size <= end) {
                    idx1.resize(size);
                    if (!ReadFully(mSource, body, idx1.data(), size)) {
                        idx1.clear();
                    }
                }
                child = body + size + (size & 1u);
            }
            if (end <= pos) {
                break;
            }
            pos = end + (end & 1u);
            if (!haveHeader) {
                return false;
            }
        }
        if (!haveHeader || mStreams.empty() || segments.empty()) {
            return false;
        }
        bool needScan = false;
        for (Stream &s : mStreams) {
            if (!s.valid) {
                continue;
            }
            if (!s.superIndex.empty()) {
                loadOpenDml(&s);
            } else if (!idx1.empty()) {
                // Filled below in one pass over idx1.
            } else {
                needScan = true;
            }
        }
        bool anyIdx1 = false;
        for (const Stream &s : mStreams) {
            anyIdx1 |= s.valid && s.superIndex.empty();
        }
        if (anyIdx1 && !idx1.empty()) {
            if (!buildFromIdx1(idx1, segments.front())) {
                needScan = true;
            }
        }
        if (needScan || (anyIdx1 && idx1.empty())) {
            scanMovi(segments);
        }
        return finish();
    }

    std::vector<Stream> &streams() { return mStreams; }
    int64_t durationUs() const { return mDurationUs; }

private:
    struct Segment {
        uint64_t start;  // first byte after the 'movi' list type
        uint64_t end;
    };

    bool parseHeaderList(uint64_t pos, uint64_t end) {
        uint32_t microsPerFrame = 0;
        while (pos + 8 <= end) {
            uint8_t ch[12];
            if (!ReadFully(mSource, pos, ch, 8)) {
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
                if (ReadFully(mSource, body, avih, 4)) {
                    microsPerFrame = Le32(avih);
                }
            } else if (id == FourCC('L', 'I', 'S', 'T') && size >= 4) {
                uint8_t type[4];
                if (!ReadFully(mSource, body, type, 4)) {
                    return false;
                }
                if (Le32(type) == FourCC('s', 't', 'r', 'l')) {
                    if (mStreams.size() >= kMaxStreams) {
                        return false;
                    }
                    Stream s;
                    parseStreamList(body + 4, body + size, &s);
                    if (s.info.video && s.info.frameRate == 0 && microsPerFrame > 0) {
                        s.info.frameRate = static_cast<int32_t>((1000000ull + microsPerFrame / 2) / microsPerFrame);
                    }
                    mStreams.push_back(std::move(s));
                }
            }
            pos = body + size + (size & 1u);
        }
        return !mStreams.empty();
    }

    void parseStreamList(uint64_t pos, uint64_t end, Stream *s) {
        uint32_t type = 0;
        while (pos + 8 <= end) {
            uint8_t ch[8];
            if (!ReadFully(mSource, pos, ch, 8)) {
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
                if (!ReadFully(mSource, body, h, sizeof(h))) {
                    return;
                }
                type = Le32(h);
                s->handler = Le32(h + 4);
                s->scale = std::max<uint32_t>(Le32(h + 20), 1);
                s->rate = std::max<uint32_t>(Le32(h + 24), 1);
                s->start = Le32(h + 28);
                s->suggested = std::min<uint32_t>(Le32(h + 36), kMaxPacketBytes);
                s->sampleSize = Le32(h + 44);
                s->info.video = type == FourCC('v', 'i', 'd', 's');
                if (s->info.video) {
                    const uint64_t fps = (static_cast<uint64_t>(s->rate) + s->scale / 2) / s->scale;
                    s->info.frameRate = static_cast<int32_t>(std::min<uint64_t>(fps, 240));
                }
            } else if (id == FourCC('s', 't', 'r', 'f')) {
                std::vector<uint8_t> f(std::min<uint32_t>(size, 1u << 20));
                if (!f.empty() && !ReadFully(mSource, body, f.data(), f.size())) {
                    return;
                }
                if (type == FourCC('v', 'i', 'd', 's') && f.size() >= 40) {
                    s->info.width = static_cast<int32_t>(Le32(f.data() + 4));
                    const int32_t rawHeight = static_cast<int32_t>(Le32(f.data() + 8));
                    // A bottom-up bitmap stores a positive height, a top-down one
                    // a negative height; INT32_MIN has no positive counterpart.
                    s->info.height = rawHeight == INT32_MIN ? 0 : std::abs(rawHeight);
                    s->compression = Le32(f.data() + 16);
                    const uint32_t headerSize = std::max<uint32_t>(Le32(f.data()), 40);
                    if (headerSize < f.size()) {
                        s->extra.assign(f.begin() + headerSize, f.end());
                    }
                    s->valid = DescribeVideo(s);
                } else if (type == FourCC('a', 'u', 'd', 's') && f.size() >= 16) {
                    s->formatTag = Le16(f.data());
                    s->info.channels = Le16(f.data() + 2);
                    s->info.sampleRate = static_cast<int32_t>(std::min<uint32_t>(Le32(f.data() + 4), INT32_MAX));
                    s->info.bitrate = static_cast<int32_t>(std::min<uint64_t>(Le32(f.data() + 8) * 8ull, INT32_MAX));
                    s->bitsPerSample = Le16(f.data() + 14);
                    if (f.size() >= 18) {
                        const uint32_t cb = Le16(f.data() + 16);
                        if (cb > 0 && 18 + cb <= f.size()) {
                            s->extra.assign(f.begin() + 18, f.begin() + 18 + cb);
                        }
                    }
                    s->valid = DescribeAudio(s);
                }
            } else if (id == FourCC('i', 'n', 'd', 'x') && size >= 24) {
                readSuperIndex(body, size, s);
            }
            pos = body + size + (size & 1u);
        }
    }

    // AVISUPERINDEX (indx, bIndexType 0): entries point at ix## standard indexes.
    // A standard index placed directly in the stream list (bIndexType 1) is
    // treated as a single-entry super index over the chunk itself.
    void readSuperIndex(uint64_t body, uint32_t size, Stream *s) {
        std::vector<uint8_t> h(std::min<uint32_t>(size, 24));
        if (!ReadFully(mSource, body, h.data(), h.size())) {
            return;
        }
        const uint16_t longs = Le16(h.data());
        const uint8_t indexType = h[3];
        const uint32_t used = Le32(h.data() + 4);
        if (indexType == 1) {
            s->superIndex.push_back({body - 8, size});
            return;
        }
        if (indexType != 0 || longs != 4 || used > kMaxPackets
                || 24ull + static_cast<uint64_t>(used) * 16 > size) {
            return;
        }
        std::vector<uint8_t> entries(static_cast<size_t>(used) * 16);
        if (!ReadFully(mSource, body + 24, entries.data(), entries.size())) {
            return;
        }
        for (uint32_t i = 0; i < used; ++i) {
            const uint8_t *e = entries.data() + static_cast<size_t>(i) * 16;
            s->superIndex.push_back({Le64(e), Le32(e + 8)});
        }
    }

    bool addPacket(Stream *s, uint64_t offset, uint32_t size, bool key) {
        const uint64_t ordinal = s->chunks++;
        if (size == 0 || size > kMaxPacketBytes || s->info.packets.size() >= kMaxPackets) {
            return true;  // a dropped frame keeps its timeline slot and has no packet
        }
        const uint64_t units = s->start + ((s->sampleSize > 0 && !s->info.video)
                ? s->bytes / s->sampleSize : ordinal);
        Packet p;
        p.offset = offset;
        p.size = size;
        p.key = s->info.video ? key : true;
        if (!UnitsToUs(units, s->scale, s->rate, &p.timeUs)) {
            s->timeOverflow = true;
            return false;
        }
        s->bytes += size;
        s->info.maxInput = std::max<size_t>(s->info.maxInput, size);
        s->info.packets.push_back(p);
        return true;
    }

    // Streams addressed by the two-digit chunk prefix.
    Stream *streamFor(const uint8_t *id) {
        if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') {
            return nullptr;
        }
        const size_t n = static_cast<size_t>(id[0] - '0') * 10 + static_cast<size_t>(id[1] - '0');
        return n < mStreams.size() && mStreams[n].valid ? &mStreams[n] : nullptr;
    }

    void loadOpenDml(Stream *s) {
        for (const SuperEntry &e : s->superIndex) {
            // The track packet cap bounds the work: once it is reached no
            // further standard index adds a sample.
            if (s->info.packets.size() >= kMaxPackets) {
                return;
            }
            uint8_t head[32];
            if (mFileSize < 32 || e.offset > mFileSize - 32 || !ReadFully(mSource, e.offset, head, sizeof(head))) {
                continue;
            }
            // ix## chunk: 8-byte header, then AVISTDINDEX.
            const uint8_t *h = head + 8;
            const uint16_t longs = Le16(h);
            const uint8_t indexType = h[3];
            const uint32_t used = Le32(h + 4);
            const uint64_t base = Le64(h + 12);
            if (indexType != 1 || longs != 2 || used > kMaxPackets) {
                continue;
            }
            std::vector<uint8_t> entries(static_cast<size_t>(used) * 8);
            if (!ReadFully(mSource, e.offset + 32, entries.data(), entries.size())) {
                continue;
            }
            for (uint32_t i = 0; i < used && s->info.packets.size() < kMaxPackets; ++i) {
                const uint8_t *ent = entries.data() + static_cast<size_t>(i) * 8;
                const uint32_t sz = Le32(ent + 4);
                const uint32_t len = sz & 0x7FFFFFFFu;
                // Offsets address the chunk data; the size's top bit marks a delta frame.
                // A position that wraps or runs past the file keeps its timeline slot
                // as a dropped frame.
                uint64_t pos = 0;
                uint64_t endPos = 0;
                const bool inFile = !__builtin_add_overflow(base, static_cast<uint64_t>(Le32(ent)), &pos)
                        && !__builtin_add_overflow(pos, static_cast<uint64_t>(len), &endPos)
                        && endPos <= mFileSize;
                if (!addPacket(s, pos, inFile ? len : 0, (sz & 0x80000000u) == 0)) {
                    return;
                }
            }
        }
    }

    // idx1 offsets are relative to the 'movi' fourcc in most files and to the
    // start of the file in the rest; the first chunk header names the base.
    bool buildFromIdx1(const std::vector<uint8_t> &idx1, const Segment &movi) {
        const size_t count = idx1.size() / 16;
        uint64_t base = movi.start - 4;
        uint8_t ch[4];
        const uint64_t first = Le32(idx1.data() + 8);
        if (!(ReadFully(mSource, base + first, ch, 4) && memcmp(ch, idx1.data(), 4) == 0)) {
            base = 0;
            if (!(ReadFully(mSource, first, ch, 4) && memcmp(ch, idx1.data(), 4) == 0)) {
                return false;
            }
        }
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *e = idx1.data() + i * 16;
            Stream *s = streamFor(e);
            if (s == nullptr || !s->superIndex.empty()) {
                continue;
            }
            const uint64_t offset = base + Le32(e + 8) + 8;
            const uint32_t size = Le32(e + 12);
            if (offset + size > mFileSize) {
                continue;
            }
            if (!addPacket(s, offset, size, (Le32(e + 4) & kIdx1KeyFrame) != 0)) {
                s->valid = false;
            }
        }
        return true;
    }

    // Without an index the chunk headers of the movi lists give the packets;
    // only the first video chunk is known to be a sync frame.
    void scanMovi(const std::vector<Segment> &segments) {
        for (Stream &s : mStreams) {
            if (s.superIndex.empty()) {
                s.info.packets.clear();
                s.chunks = 0;
                s.bytes = 0;
            }
        }
        uint32_t guard = 0;
        for (const Segment &seg : segments) {
            uint64_t pos = seg.start;
            while (pos + 8 <= seg.end && guard++ < kMaxPackets) {
                uint8_t ch[8];
                if (!ReadFully(mSource, pos, ch, 8)) {
                    break;
                }
                const uint32_t id = Le32(ch);
                const uint32_t size = Le32(ch + 4);
                const uint64_t body = pos + 8;
                if (id == FourCC('L', 'I', 'S', 'T')) {
                    pos = body + 4;  // descend into a 'rec ' list
                    continue;
                }
                if (body + size > seg.end) {
                    break;
                }
                Stream *s = streamFor(ch);
                if (s != nullptr && s->superIndex.empty() && !addPacket(s, body, size, false)) {
                    s->valid = false;
                }
                pos = body + size + (size & 1u);
            }
        }
    }

    bool finish() {
        bool any = false;
        for (Stream &s : mStreams) {
            if (!s.valid || s.timeOverflow || s.info.packets.empty()) {
                s.valid = false;
                continue;
            }
            any = true;
            // The first video chunk always starts a decodable sequence.
            if (s.info.video) {
                s.info.packets.front().key = true;
            }
            s.info.maxInput = std::max<size_t>(std::max<size_t>(s.info.maxInput, s.suggested), 4096);
            if (s.info.transform == Transform::kAvcAnnexB) {
                s.info.maxInput = AnnexBMaxSize(s.info.maxInput, s.info.nalLengthSize);
            }
            int64_t end = s.info.packets.back().timeUs;
            int64_t frameUs = 0;
            if (s.info.video && UnitsToUs(1, s.scale, s.rate, &frameUs) &&
                __builtin_add_overflow(end, frameUs, &end)) {
                end = s.info.packets.back().timeUs;
            }
            if (!s.info.video) {
                // An audio track ends where its last chunk's units end: the byte
                // count over the sample size for fixed-size samples, else the
                // chunk count.
                int64_t audioEnd = 0;
                const uint64_t units = s.start + (s.sampleSize > 0 ? s.bytes / s.sampleSize : s.chunks);
                if (UnitsToUs(units, s.scale, s.rate, &audioEnd)) {
                    end = std::max(end, audioEnd);
                }
            }
            mDurationUs = std::max(mDurationUs, end);
        }
        return any;
    }

    ByteSource *mSource = nullptr;
    uint64_t mFileSize = UINT64_MAX;
    std::vector<Stream> mStreams;
    int64_t mDurationUs = 0;
};

}  // namespace

bool ParseAvi(ByteSource *source, Container *out) {
    AviFile file;
    if (!file.parse(source)) {
        return false;
    }
    out->tracks.clear();
    for (Stream &s : file.streams()) {
        if (s.valid) {
            s.info.durationUs = file.durationUs();
            out->tracks.push_back(std::move(s.info));
        }
    }
    out->durationUs = file.durationUs();
    return !out->tracks.empty();
}

}  // namespace a11
