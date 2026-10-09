/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ASF container parser: header objects, fixed-size data packets and media
 * object reassembly, written from the Advanced Systems Format specification.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <vector>

namespace asf {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Returns the byte count read, or a negative value on error.
    virtual ssize_t readAt(int64_t offset, void *data, size_t size) = 0;
};

enum class StreamKind : uint8_t { kAudio, kVideo, kOther };

struct StreamInfo {
    int number = 0;
    StreamKind kind = StreamKind::kOther;
    bool encrypted = false;
    // Audio: the WAVEFORMATEX type-specific data, verbatim.
    // Video: the codec-specific bytes that follow the BITMAPINFOHEADER.
    std::vector<uint8_t> codecData;
    std::vector<uint8_t> waveFormat;
    uint16_t formatTag = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint32_t avgBytesPerSec = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 0;
    uint32_t fourcc = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Audio Spread error correction; span > 1 interleaves each object.
    uint8_t spreadSpan = 0;
    uint16_t spreadPacketLength = 0;
    uint16_t spreadChunkLength = 0;
    // Extended Stream Properties.
    uint32_t bitrate = 0;
    uint32_t maxObjectSize = 0;
    uint64_t avgTimePerFrame100ns = 0;
    // Stream Properties Time Offset, added to every presentation time.
    uint64_t timeOffset100ns = 0;
    // Simple Index Object of a video stream: the n-th index object in the
    // file belongs to the n-th video stream.
    struct IndexEntry {
        uint32_t packet;
        uint16_t count;
    };
    std::vector<IndexEntry> index;
    uint64_t indexInterval100ns = 0;
};

class AsfFile {
public:
    // Parses the Header Object, the Data Object header and, when present at
    // the end of the data, the Simple Index Object. Returns false for a file
    // this parser cannot demux.
    bool parse(ByteSource *source, int64_t fileSize);

    const std::vector<StreamInfo> &streams() const { return mStreams; }
    uint32_t packetSize() const { return mPacketSize; }
    int64_t dataOffset() const { return mDataOffset; }
    uint64_t packetCount() const { return mPacketCount; }
    uint64_t prerollMs() const { return mPrerollMs; }
    int64_t durationUs() const { return mDurationUs; }

private:
    bool parseHeaderObjects(ByteSource *source, int64_t offset, int64_t end, bool extension);
    bool parseStreamProperties(const uint8_t *data, size_t size);
    bool parseExtendedStreamProperties(const uint8_t *data, size_t size);
    void parseSimpleIndex(ByteSource *source, int64_t offset, int64_t fileSize);
    StreamInfo *findStream(int number);

    std::vector<StreamInfo> mStreams;
    uint32_t mPacketSize = 0;
    int64_t mDataOffset = 0;
    uint64_t mPacketCount = 0;
    uint64_t mPrerollMs = 0;
    int64_t mDurationUs = 0;
    // Extended Stream Properties seen before their Stream Properties Object.
    std::vector<StreamInfo> mPendingExtended;
};

struct MediaObject {
    std::vector<uint8_t> data;
    int64_t timeUs = 0;
    bool keyFrame = false;
};

// Reads the media objects of one stream in file order.
class StreamReader {
public:
    StreamReader(const AsfFile &file, ByteSource *source, const StreamInfo &stream);

    // Returns false at the end of the data or on a read error.
    bool next(MediaObject *object);
    // Positions the reader at the packet that holds the key frame at or
    // before timeUs (Simple Index Object) or at the last packet sent at or
    // before timeUs plus the preroll (send-time bisection).
    // With nextSync, objects presented before timeUs are dropped, so the
    // first object returned is the first sync object at or after timeUs.
    void seek(int64_t timeUs, bool nextSync);

private:
    bool readPacket(uint64_t packet);
    bool packetSendTime(uint64_t packet, uint32_t *sendTimeMs);
    void addFragment(uint32_t objectNumber, uint32_t offset, uint32_t objectSize,
            uint32_t presentationMs, bool key, const uint8_t *data, size_t size);
    void emit(std::vector<uint8_t> &&data, uint32_t presentationMs, bool key);
    void descramble(std::vector<uint8_t> *data) const;

    const AsfFile &mFile;
    ByteSource *mSource;
    const StreamInfo &mStream;
    uint64_t mNextPacket = 0;
    std::vector<uint8_t> mPacket;
    std::vector<MediaObject> mReady;
    size_t mReadyHead = 0;
    bool mAssembling = false;
    uint32_t mObjectNumber = 0;
    uint32_t mObjectSize = 0;
    uint32_t mObjectPresentationMs = 0;
    bool mObjectKey = false;
    std::vector<uint8_t> mObject;
    bool mNeedKey = false;
    bool mDropBeforeTarget = false;
    int64_t mTargetUs = 0;
};

}  // namespace asf
