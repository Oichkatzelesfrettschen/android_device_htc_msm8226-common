/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ASF container parser: header objects, fixed-size data packets and media
 * object reassembly, written from the Advanced Systems Format specification.
 */

#include "AsfParser.h"

#include <string.h>

#include <algorithm>

namespace asf {

namespace {

struct Guid {
    uint8_t b[16];
};

// GUIDs in their on-disk byte order (the first three fields little-endian).
constexpr Guid kHeaderObject = {{0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C}};
constexpr Guid kDataObject = {{0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                               0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C}};
constexpr Guid kSimpleIndexObject = {{0x90, 0x08, 0x00, 0x33, 0xB1, 0xE5, 0xCF, 0x11,
                                      0x89, 0xF4, 0x00, 0xA0, 0xC9, 0x03, 0x49, 0xCB}};
constexpr Guid kFilePropertiesObject = {{0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                         0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65}};
constexpr Guid kStreamPropertiesObject = {{0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                           0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65}};
constexpr Guid kHeaderExtensionObject = {{0xB5, 0x03, 0xBF, 0x5F, 0x2E, 0xA9, 0xCF, 0x11,
                                          0x8E, 0xE3, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65}};
constexpr Guid kExtendedStreamPropertiesObject = {{0xCB, 0xA5, 0xE6, 0x14, 0x72, 0xC6,
                                                   0x32, 0x43, 0x83, 0x99, 0xA9, 0x69,
                                                   0x52, 0x06, 0x5B, 0x5A}};
constexpr Guid kAudioMedia = {{0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11,
                               0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B}};
constexpr Guid kVideoMedia = {{0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11,
                               0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B}};
constexpr Guid kAudioSpread = {{0x50, 0xCD, 0xC3, 0xBF, 0x8F, 0x61, 0xCF, 0x11,
                                0x8B, 0xB2, 0x00, 0xAA, 0x00, 0xB4, 0xE2, 0x20}};

constexpr size_t kObjectHeaderSize = 24;     // GUID + QWORD size
constexpr int64_t kObjectHeaderOffset = (int64_t)kObjectHeaderSize;
constexpr uint64_t kMaxHeaderSize = 1 << 24;  // header objects are read whole
constexpr uint32_t kMaxPacketSize = 1 << 20;
constexpr uint32_t kMaxObjectSize = 16 << 20;
// Packet numbers stay below 2^32 so packet * packetSize fits an int64_t.
constexpr uint64_t kMaxPacketCount = 1ull << 32;
// A preroll beyond a day marks a corrupt File Properties Object.
constexpr uint64_t kMaxPrerollMs = 24ull * 3600 * 1000;

bool isGuid(const uint8_t *p, const Guid &g) {
    return memcmp(p, g.b, sizeof(g.b)) == 0;
}

uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
            | ((uint32_t)p[3] << 24);
}

uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

bool readFully(ByteSource *source, int64_t offset, void *data, size_t size) {
    return source->readAt(offset, data, size) == (ssize_t)size;
}

// Bounds-checked little-endian cursor over a byte range.
class Cursor {
public:
    Cursor(const uint8_t *data, size_t size) : mData(data), mSize(size) {}

    bool ok() const { return mOk; }
    size_t remaining() const { return mOk ? mSize - mPos : 0; }
    size_t position() const { return mPos; }
    const uint8_t *here() const { return mData + mPos; }

    bool skip(size_t n) {
        if (!mOk || n > mSize - mPos) {
            mOk = false;
            return false;
        }
        mPos += n;
        return true;
    }

    uint32_t u8() { return take(1) ? mData[mPos - 1] : 0; }
    uint32_t u16() { return take(2) ? le16(mData + mPos - 2) : 0; }
    uint32_t u32() { return take(4) ? le32(mData + mPos - 4) : 0; }
    uint64_t u64() { return take(8) ? le64(mData + mPos - 8) : 0; }

    // Field whose width a two-bit length type selects: 0 absent, 1 BYTE,
    // 2 WORD, 3 DWORD.
    uint32_t sized(unsigned type) {
        switch (type & 3) {
            case 1: return u8();
            case 2: return u16();
            case 3: return u32();
            default: return 0;
        }
    }

private:
    bool take(size_t n) { return skip(n); }

    const uint8_t *mData;
    size_t mSize;
    size_t mPos = 0;
    bool mOk = true;
};

}  // namespace

StreamInfo *AsfFile::findStream(int number) {
    for (StreamInfo &s : mStreams) {
        if (s.number == number) {
            return &s;
        }
    }
    return nullptr;
}

bool AsfFile::parseStreamProperties(const uint8_t *data, size_t size) {
    Cursor c(data, size);
    if (c.remaining() < 54) {
        return false;
    }
    const uint8_t *streamType = c.here();
    c.skip(16);
    const uint8_t *errorCorrectionType = c.here();
    c.skip(16);
    c.u64();  // time offset
    uint32_t typeSpecificLength = c.u32();
    uint32_t errorCorrectionLength = c.u32();
    uint32_t flags = c.u16();
    c.u32();  // reserved
    if (!c.ok() || typeSpecificLength > c.remaining()) {
        return false;
    }
    const uint8_t *typeSpecific = c.here();
    c.skip(typeSpecificLength);
    if (errorCorrectionLength > c.remaining()) {
        return false;
    }
    const uint8_t *errorCorrection = c.here();

    StreamInfo info;
    info.number = static_cast<int>(flags & 0x7f);
    info.encrypted = (flags & 0x8000) != 0;
    if (info.number == 0 || findStream(info.number) != nullptr) {
        return true;  // stream 0 is invalid; a duplicate keeps the first
    }

    if (isGuid(streamType, kAudioMedia)) {
        info.kind = StreamKind::kAudio;
        if (typeSpecificLength < 18) {
            return false;
        }
        Cursor w(typeSpecific, typeSpecificLength);
        info.formatTag = w.u16();
        info.channels = w.u16();
        info.sampleRate = w.u32();
        info.avgBytesPerSec = w.u32();
        info.blockAlign = w.u16();
        info.bitsPerSample = w.u16();
        uint32_t cbSize = w.u16();
        if (cbSize > w.remaining()) {
            cbSize = w.remaining();
        }
        info.waveFormat.assign(typeSpecific, typeSpecific + 18 + cbSize);
        info.codecData.assign(typeSpecific + 18, typeSpecific + 18 + cbSize);
        info.bitrate = (uint32_t)std::min<uint64_t>((uint64_t)info.avgBytesPerSec * 8, UINT32_MAX);
        if (isGuid(errorCorrectionType, kAudioSpread) && errorCorrectionLength >= 5) {
            Cursor e(errorCorrection, errorCorrectionLength);
            info.spreadSpan = e.u8();
            info.spreadPacketLength = e.u16();
            info.spreadChunkLength = e.u16();
        }
    } else if (isGuid(streamType, kVideoMedia)) {
        info.kind = StreamKind::kVideo;
        Cursor v(typeSpecific, typeSpecificLength);
        info.width = v.u32();
        info.height = v.u32();
        v.u8();  // reserved flags
        uint32_t formatDataSize = v.u16();
        if (!v.ok() || formatDataSize < 40 || formatDataSize > v.remaining()) {
            return false;
        }
        const uint8_t *bih = v.here();
        Cursor b(bih, formatDataSize);
        uint32_t bihSize = b.u32();
        uint32_t biWidth = b.u32();
        uint32_t biHeight = b.u32();
        b.u16();  // planes
        b.u16();  // bit count
        info.fourcc = b.u32();
        if (biWidth != 0 && biHeight != 0) {
            info.width = biWidth;
            info.height = biHeight & 0x7fffffff;
        }
        size_t extraStart = 40;
        size_t extraEnd = std::min<size_t>(formatDataSize, std::max<uint32_t>(bihSize, 40));
        if (extraEnd > extraStart) {
            info.codecData.assign(bih + extraStart, bih + extraEnd);
        }
    }

    // Extended Stream Properties may precede the stream they describe.
    for (auto it = mPendingExtended.begin(); it != mPendingExtended.end(); ++it) {
        if (it->number == info.number) {
            info.bitrate = it->bitrate ? it->bitrate : info.bitrate;
            info.maxObjectSize = it->maxObjectSize;
            info.avgTimePerFrame100ns = it->avgTimePerFrame100ns;
            mPendingExtended.erase(it);
            break;
        }
    }
    mStreams.push_back(std::move(info));
    return true;
}

bool AsfFile::parseExtendedStreamProperties(const uint8_t *data, size_t size) {
    Cursor c(data, size);
    c.u64();  // start time
    c.u64();  // end time
    uint32_t bitrate = c.u32();
    c.u32();  // buffer size
    c.u32();  // initial buffer fullness
    c.u32();  // alternate data bitrate
    c.u32();  // alternate buffer size
    c.u32();  // alternate initial buffer fullness
    // Objects above kMaxObjectSize are dropped during reassembly, so a larger
    // declared bound never sizes a buffer.
    uint32_t maxObjectSize = std::min(c.u32(), kMaxObjectSize);
    c.u32();  // flags
    int number = static_cast<int>(c.u16() & 0x7f);
    c.u16();  // stream language ID index
    uint64_t avgTimePerFrame = c.u64();
    uint32_t nameCount = c.u16();
    uint32_t extensionCount = c.u16();
    for (uint32_t i = 0; i < nameCount && c.ok(); ++i) {
        c.u16();  // language ID index
        c.skip(c.u16());
    }
    for (uint32_t i = 0; i < extensionCount && c.ok(); ++i) {
        c.skip(16);  // extension system GUID
        c.u16();     // extension data size
        c.skip(c.u32());
    }
    if (!c.ok()) {
        return false;
    }
    // An embedded Stream Properties Object declares the stream here.
    if (c.remaining() >= kObjectHeaderSize && isGuid(c.here(), kStreamPropertiesObject)) {
        uint64_t objectSize = le64(c.here() + 16);
        if (objectSize >= kObjectHeaderSize && objectSize <= c.remaining()) {
            if (!parseStreamProperties(c.here() + kObjectHeaderSize,
                    (size_t)objectSize - kObjectHeaderSize)) {
                return false;
            }
        }
    }
    StreamInfo *s = findStream(number);
    if (s != nullptr) {
        if (bitrate) {
            s->bitrate = bitrate;
        }
        s->maxObjectSize = maxObjectSize;
        s->avgTimePerFrame100ns = avgTimePerFrame;
    } else {
        StreamInfo pending;
        pending.number = number;
        pending.bitrate = bitrate;
        pending.maxObjectSize = maxObjectSize;
        pending.avgTimePerFrame100ns = avgTimePerFrame;
        mPendingExtended.push_back(std::move(pending));
    }
    return true;
}

bool AsfFile::parseHeaderObjects(ByteSource *source, int64_t offset, int64_t end,
        bool extension) {
    while (offset + kObjectHeaderOffset <= end) {
        uint8_t head[kObjectHeaderSize];
        if (!readFully(source, offset, head, sizeof(head))) {
            return false;
        }
        uint64_t objectSize = le64(head + 16);
        if (objectSize < kObjectHeaderSize || objectSize > (uint64_t)(end - offset)) {
            return false;
        }
        size_t bodySize = (size_t)objectSize - kObjectHeaderSize;
        std::vector<uint8_t> body(bodySize);
        if (bodySize && !readFully(source, offset + kObjectHeaderOffset, body.data(), bodySize)) {
            return false;
        }

        if (!extension && isGuid(head, kFilePropertiesObject)) {
            if (bodySize < 80) {
                return false;
            }
            Cursor c(body.data(), bodySize);
            c.skip(16);  // file ID
            c.u64();     // file size
            c.u64();     // creation date
            c.u64();     // data packets count
            uint64_t playDuration = c.u64();
            c.u64();     // send duration
            mPrerollMs = c.u64();
            c.u32();     // flags
            uint32_t minPacket = c.u32();
            uint32_t maxPacket = c.u32();
            // Data packets have one fixed size in a file this parser reads.
            if (minPacket != maxPacket || minPacket == 0 || minPacket > kMaxPacketSize) {
                return false;
            }
            if (mPrerollMs > kMaxPrerollMs) {
                return false;
            }
            mPacketSize = minPacket;
            int64_t durationUs = (int64_t)(playDuration / 10) - (int64_t)mPrerollMs * 1000;
            mDurationUs = durationUs > 0 ? durationUs : 0;
        } else if (isGuid(head, kStreamPropertiesObject)) {
            // The Header Extension may carry Stream Properties Objects too.
            if (!parseStreamProperties(body.data(), bodySize)) {
                return false;
            }
        } else if (!extension && isGuid(head, kHeaderExtensionObject)) {
            if (bodySize < 22) {
                return false;
            }
            uint32_t dataSize = le32(body.data() + 18);
            int64_t start = offset + kObjectHeaderOffset + 22;
            if (dataSize > bodySize - 22
                    || !parseHeaderObjects(source, start, start + dataSize, true)) {
                return false;
            }
        } else if (extension && isGuid(head, kExtendedStreamPropertiesObject)) {
            if (!parseExtendedStreamProperties(body.data(), bodySize)) {
                return false;
            }
        }
        offset += (int64_t)objectSize;
    }
    return true;
}

void AsfFile::parseSimpleIndex(ByteSource *source, int64_t offset, int64_t fileSize) {
    while (offset + kObjectHeaderOffset <= fileSize) {
        uint8_t head[kObjectHeaderSize];
        if (!readFully(source, offset, head, sizeof(head))) {
            return;
        }
        uint64_t objectSize = le64(head + 16);
        if (objectSize < kObjectHeaderSize || objectSize > (uint64_t)(fileSize - offset)) {
            return;
        }
        if (isGuid(head, kSimpleIndexObject) && objectSize >= kObjectHeaderSize + 32) {
            uint8_t fixed[32];
            if (!readFully(source, offset + kObjectHeaderOffset, fixed, sizeof(fixed))) {
                return;
            }
            uint64_t interval = le64(fixed + 16);
            uint32_t count = le32(fixed + 28);
            uint64_t available = (objectSize - kObjectHeaderSize - 32) / 6;
            if (interval == 0 || count == 0 || count > available || count > (1u << 22)) {
                return;
            }
            std::vector<uint8_t> raw((size_t)count * 6);
            if (!readFully(source, offset + kObjectHeaderOffset + 32, raw.data(), raw.size())) {
                return;
            }
            mIndex.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                mIndex[i].packet = le32(&raw[(size_t)i * 6]);
                mIndex[i].count = le16(&raw[(size_t)i * 6 + 4]);
            }
            mIndexInterval100ns = interval;
            return;
        }
        offset += (int64_t)objectSize;
    }
}

bool AsfFile::parse(ByteSource *source, int64_t fileSize) {
    uint8_t head[30];
    if (!readFully(source, 0, head, sizeof(head)) || !isGuid(head, kHeaderObject)) {
        return false;
    }
    uint64_t headerSize = le64(head + 16);
    if (headerSize < sizeof(head) || headerSize > kMaxHeaderSize
            || (fileSize > 0 && headerSize > (uint64_t)fileSize)) {
        return false;
    }
    if (!parseHeaderObjects(source, sizeof(head), (int64_t)headerSize, false)) {
        return false;
    }
    if (mPacketSize == 0 || mStreams.empty()) {
        return false;
    }

    uint8_t data[50];
    int64_t dataObject = (int64_t)headerSize;
    if (!readFully(source, dataObject, data, sizeof(data)) || !isGuid(data, kDataObject)) {
        return false;
    }
    uint64_t dataObjectSize = le64(data + 16);
    mPacketCount = le64(data + 40);
    mDataOffset = dataObject + (int64_t)sizeof(data);
    if (fileSize > 0 && mDataOffset > fileSize) {
        return false;
    }
    if (fileSize > 0) {
        uint64_t available = (uint64_t)(fileSize - mDataOffset) / mPacketSize;
        // Broadcast and truncated files leave the count zero or too large.
        if (mPacketCount == 0 || mPacketCount > available) {
            mPacketCount = available;
        }
    }
    if (mPacketCount > kMaxPacketCount) {
        mPacketCount = kMaxPacketCount;
    }
    if (fileSize > 0) {
        if (dataObjectSize >= sizeof(data)
                && dataObjectSize <= (uint64_t)(fileSize - dataObject)) {
            parseSimpleIndex(source, dataObject + (int64_t)dataObjectSize, fileSize);
        }
    }
    return mPacketCount > 0;
}

StreamReader::StreamReader(const AsfFile &file, ByteSource *source, const StreamInfo &stream)
    : mFile(file), mSource(source), mStream(stream) {}

void StreamReader::descramble(std::vector<uint8_t> *data) const {
    // Audio Spread: an object of span * virtual-packet-length bytes is
    // written as chunks in column order across span rows; reading row by row
    // restores the codec order.
    uint32_t span = mStream.spreadSpan;
    uint32_t packetLength = mStream.spreadPacketLength;
    uint32_t chunk = mStream.spreadChunkLength;
    if (span <= 1 || chunk == 0 || packetLength == 0 || packetLength % chunk != 0
            || data->size() != (size_t)span * packetLength) {
        return;
    }
    uint32_t chunksPerPacket = packetLength / chunk;
    std::vector<uint8_t> out(data->size());
    size_t chunks = data->size() / chunk;
    for (size_t i = 0; i < chunks; ++i) {
        size_t row = i / span;
        size_t column = i % span;
        size_t source = row + column * chunksPerPacket;
        memcpy(&out[i * chunk], &(*data)[source * chunk], chunk);
    }
    data->swap(out);
}

void StreamReader::emit(std::vector<uint8_t> &&data, uint32_t presentationMs, bool key) {
    bool video = mStream.kind == StreamKind::kVideo;
    if (video && mNeedKey) {
        if (!key) {
            return;
        }
        mNeedKey = false;
    }
    MediaObject object;
    object.data = std::move(data);
    if (!video) {
        descramble(&object.data);
    }
    int64_t timeUs = ((int64_t)presentationMs - (int64_t)mFile.prerollMs()) * 1000;
    object.timeUs = timeUs > 0 ? timeUs : 0;
    object.keyFrame = video ? key : true;
    mReady.push_back(std::move(object));
}

void StreamReader::addFragment(uint32_t objectNumber, uint32_t offset, uint32_t objectSize,
        uint32_t presentationMs, bool key, const uint8_t *data, size_t size) {
    if (offset == 0) {
        mAssembling = objectSize > 0 && objectSize <= kMaxObjectSize;
        mObjectNumber = objectNumber;
        mObjectSize = objectSize;
        mObjectPresentationMs = presentationMs;
        mObjectKey = key;
        mObject.clear();
    } else if (!mAssembling || objectNumber != mObjectNumber || offset != mObject.size()) {
        // A lost or reordered fragment drops the rest of the object.
        mAssembling = false;
        return;
    }
    if (!mAssembling || size > mObjectSize - mObject.size()) {
        mAssembling = false;
        return;
    }
    mObject.insert(mObject.end(), data, data + size);
    if (mObject.size() == mObjectSize) {
        mAssembling = false;
        emit(std::move(mObject), mObjectPresentationMs, mObjectKey);
        mObject = std::vector<uint8_t>();
    }
}

bool StreamReader::readPacket(uint64_t packet) {
    uint32_t packetSize = mFile.packetSize();
    mPacket.resize(packetSize);
    int64_t offset = mFile.dataOffset() + (int64_t)(packet * packetSize);
    if (!readFully(mSource, offset, mPacket.data(), packetSize)) {
        return false;
    }
    Cursor c(mPacket.data(), packetSize);
    uint32_t flags = c.u8();
    if (flags & 0x80) {
        // Error correction data: low nibble is its length; the length type
        // bits must be zero.
        if (flags & 0x60) {
            return true;
        }
        c.skip(flags & 0x0f);
        flags = c.u8();
    }
    uint32_t property = c.u8();
    uint32_t packetLength = c.sized(flags >> 5);
    c.sized(flags >> 1);  // sequence
    uint32_t padding = c.sized(flags >> 3);
    c.u32();  // send time
    c.u16();  // duration
    if (!c.ok()) {
        return true;
    }
    if (packetLength == 0 || packetLength > packetSize) {
        packetLength = packetSize;
    }
    if (padding > packetLength) {
        return true;
    }
    size_t end = packetLength - padding;
    if (c.position() > end) {
        return true;
    }

    bool multiple = flags & 0x01;
    uint32_t payloads = 1;
    unsigned payloadLengthType = 0;
    if (multiple) {
        uint32_t payloadFlags = c.u8();
        payloads = payloadFlags & 0x3f;
        payloadLengthType = payloadFlags >> 6;
    }
    for (uint32_t i = 0; i < payloads && c.ok(); ++i) {
        uint32_t streamByte = c.u8();
        uint32_t objectNumber = c.sized(property >> 4);
        uint32_t offsetOrTime = c.sized(property >> 2);
        uint32_t replicatedLength = c.sized(property);
        if (!c.ok() || replicatedLength > c.remaining()) {
            return true;
        }
        const uint8_t *replicated = c.here();
        c.skip(replicatedLength);
        size_t payloadLength;
        if (multiple) {
            payloadLength = c.sized(payloadLengthType);
        } else {
            payloadLength = c.position() <= end ? end - c.position() : 0;
        }
        if (!c.ok() || c.position() > end || payloadLength > end - c.position()) {
            return true;
        }
        const uint8_t *payload = c.here();
        c.skip(payloadLength);

        int number = static_cast<int>(streamByte & 0x7f);
        bool key = (streamByte & 0x80) != 0;
        if (number != mStream.number) {
            continue;
        }
        if (replicatedLength == 1) {
            // Compressed payload: the offset field carries the presentation
            // time, the replicated byte the time delta, and the payload holds
            // whole objects each prefixed by a one-byte size.
            uint64_t delta = replicated[0];
            uint64_t time = offsetOrTime;
            size_t pos = 0;
            while (pos < payloadLength) {
                size_t size = payload[pos++];
                if (size > payloadLength - pos) {
                    break;
                }
                emit(std::vector<uint8_t>(payload + pos, payload + pos + size),
                        (uint32_t)std::min<uint64_t>(time, UINT32_MAX), key);
                pos += size;
                time += delta;
            }
        } else if (replicatedLength >= 8) {
            addFragment(objectNumber, offsetOrTime, le32(replicated), le32(replicated + 4), key,
                    payload, payloadLength);
        }
    }
    return true;
}

bool StreamReader::next(MediaObject *object) {
    while (mReadyHead >= mReady.size()) {
        mReady.clear();
        mReadyHead = 0;
        if (mNextPacket >= mFile.packetCount()) {
            return false;
        }
        if (!readPacket(mNextPacket++)) {
            return false;
        }
    }
    *object = std::move(mReady[mReadyHead++]);
    return true;
}

bool StreamReader::packetSendTime(uint64_t packet, uint32_t *sendTimeMs) {
    uint8_t head[32];
    size_t size = std::min<size_t>(sizeof(head), mFile.packetSize());
    int64_t offset = mFile.dataOffset() + (int64_t)(packet * mFile.packetSize());
    if (!readFully(mSource, offset, head, size)) {
        return false;
    }
    Cursor c(head, size);
    uint32_t flags = c.u8();
    if (flags & 0x80) {
        c.skip(flags & 0x0f);
        flags = c.u8();
    }
    c.u8();  // property flags
    c.sized(flags >> 5);
    c.sized(flags >> 1);
    c.sized(flags >> 3);
    *sendTimeMs = c.u32();
    return c.ok();
}

void StreamReader::seek(int64_t timeUs) {
    mReady.clear();
    mReadyHead = 0;
    mAssembling = false;
    mObject.clear();
    mNeedKey = mStream.kind == StreamKind::kVideo;
    // Clamping to about 35 years keeps the 100 ns arithmetic in range.
    timeUs = std::max<int64_t>(0, std::min<int64_t>(timeUs, INT64_C(1) << 50));

    // Index entries sit at multiples of the interval on the presentation
    // clock, which includes the preroll.
    const std::vector<IndexEntry> &index = mFile.index();
    if (mStream.kind == StreamKind::kVideo && !index.empty()) {
        uint64_t presentation100ns = (uint64_t)timeUs * 10 + mFile.prerollMs() * 10000;
        uint64_t slot = presentation100ns / mFile.indexInterval100ns();
        if (slot >= index.size()) {
            slot = index.size() - 1;
        }
        mNextPacket = std::min<uint64_t>(index[slot].packet, mFile.packetCount());
        return;
    }

    // A packet is sent one preroll ahead of its presentation, so send times
    // run on the media clock without the preroll.
    uint64_t target = (uint64_t)(timeUs / 1000);
    // Send times grow with the packet number; find the last packet sent at
    // or before the target.
    uint64_t lo = 0;
    uint64_t hi = mFile.packetCount();
    while (lo + 1 < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        uint32_t sendTime;
        if (!packetSendTime(mid, &sendTime)) {
            hi = mid;
            continue;
        }
        if (sendTime <= target) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    mNextPacket = lo;
}

}  // namespace asf
