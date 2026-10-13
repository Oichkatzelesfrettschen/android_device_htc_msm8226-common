/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for bitstream parsers, bounded sample transformation, and AVI
 * header edge cases that ffmpeg does not generate: stream start offsets,
 * INT32_MIN height, timestamp overflow, oversized scale values.
 */

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#include "Codecs.h"
#include "Tracks.h"

#if defined(A11_CONTAINER_ALLOCATION_PROBE)
#if defined(__SANITIZE_ADDRESS__)
#error "Allocation counting requires a separate build from AddressSanitizer"
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#error "Allocation counting requires a separate build from AddressSanitizer"
#endif
#endif

// Counts C++ heap payloads after constructing the input fixtures. The reader
// object includes its 16 KiB read window; counters exclude parser tables,
// allocator metadata, and runtime memory. The prefixed allocation header
// requires ordinary allocation in the separate AddressSanitizer build.
namespace allocation_probe {

struct alignas(std::max_align_t) Header {
    size_t bytes;
};
size_t liveBytes = 0;
size_t peakBytes = 0;

void *Allocate(size_t bytes) {
    if (bytes > SIZE_MAX - sizeof(Header)) {
        throw std::bad_alloc();
    }
    auto *header = static_cast<Header *>(std::malloc(sizeof(Header) + bytes));
    if (header == nullptr) {
        throw std::bad_alloc();
    }
    header->bytes = bytes;
    liveBytes += bytes;
    peakBytes = std::max(peakBytes, liveBytes);
    return header + 1;
}

void Free(void *address) noexcept {
    if (address != nullptr) {
        auto *header = static_cast<Header *>(address) - 1;
        liveBytes -= header->bytes;
        std::free(header);
    }
}

}  // namespace allocation_probe

void *operator new(size_t bytes) { return allocation_probe::Allocate(bytes); }
void *operator new[](size_t bytes) { return allocation_probe::Allocate(bytes); }
void operator delete(void *address) noexcept { allocation_probe::Free(address); }
void operator delete[](void *address) noexcept { allocation_probe::Free(address); }
void operator delete(void *address, size_t) noexcept { allocation_probe::Free(address); }
void operator delete[](void *address, size_t) noexcept { allocation_probe::Free(address); }
#endif

namespace {

int gFailures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++gFailures;                                                     \
        }                                                                    \
    } while (0)

class MemorySource : public a11::ByteSource {
public:
    explicit MemorySource(std::vector<uint8_t> d) : mData(std::move(d)) {}
    ssize_t readAt(int64_t offset, void *data, size_t size) override {
        if (offset < 0 || static_cast<uint64_t>(offset) >= mData.size()) {
            return 0;
        }
        const size_t n = std::min<size_t>(size, mData.size() - static_cast<size_t>(offset));
        if (n != 0) {
            memcpy(data, mData.data() + offset, n);
        }
        return static_cast<ssize_t>(n);
    }
    int64_t size() override { return static_cast<int64_t>(mData.size()); }

private:
    std::vector<uint8_t> mData;
};

void Put32(std::vector<uint8_t> *v, uint32_t x) {
    for (int i = 0; i < 4; ++i) {
        v->push_back(static_cast<uint8_t>(x >> (8 * i)));
    }
}
void Put16(std::vector<uint8_t> *v, uint16_t x) {
    v->push_back(static_cast<uint8_t>(x));
    v->push_back(static_cast<uint8_t>(x >> 8));
}
void PutTag(std::vector<uint8_t> *v, const char *t) { v->insert(v->end(), t, t + 4); }

std::vector<uint8_t> Chunk(const char *id, const std::vector<uint8_t> &body) {
    std::vector<uint8_t> c;
    PutTag(&c, id);
    Put32(&c, static_cast<uint32_t>(body.size()));
    c.insert(c.end(), body.begin(), body.end());
    if (body.size() & 1) {
        c.push_back(0);
    }
    return c;
}

std::vector<uint8_t> List(const char *type, const std::vector<uint8_t> &body) {
    std::vector<uint8_t> inner;
    PutTag(&inner, type);
    inner.insert(inner.end(), body.begin(), body.end());
    return Chunk("LIST", inner);
}

// A one-video-stream AVI with `frames` chunks of 8 bytes.
std::vector<uint8_t> MakeAvi(uint32_t scale, uint32_t rate, uint32_t start, int32_t height,
        unsigned frames) {
    std::vector<uint8_t> strh;
    PutTag(&strh, "vids");
    PutTag(&strh, "DIV3");
    Put32(&strh, 0);
    Put32(&strh, 0);
    Put32(&strh, 0);
    Put32(&strh, scale);
    Put32(&strh, rate);
    Put32(&strh, start);
    Put32(&strh, frames);
    Put32(&strh, 0);
    Put32(&strh, 0);
    Put32(&strh, 0);
    Put32(&strh, 0);
    Put32(&strh, 0);
    std::vector<uint8_t> strf;
    Put32(&strf, 40);
    Put32(&strf, 64);
    Put32(&strf, static_cast<uint32_t>(height));
    Put16(&strf, 1);
    Put16(&strf, 24);
    PutTag(&strf, "DIV3");
    for (int i = 0; i < 5; ++i) {
        Put32(&strf, 0);
    }
    std::vector<uint8_t> strl = Chunk("strh", strh);
    std::vector<uint8_t> f = Chunk("strf", strf);
    strl.insert(strl.end(), f.begin(), f.end());
    std::vector<uint8_t> avih(56, 0);
    std::vector<uint8_t> hdrl = Chunk("avih", avih);
    std::vector<uint8_t> sl = List("strl", strl);
    hdrl.insert(hdrl.end(), sl.begin(), sl.end());
    std::vector<uint8_t> movi;
    std::vector<uint8_t> idx1;
    for (unsigned i = 0; i < frames; ++i) {
        PutTag(&idx1, "00dc");
        Put32(&idx1, 0x10);
        Put32(&idx1, static_cast<uint32_t>(4 + movi.size()));
        Put32(&idx1, 8);
        std::vector<uint8_t> c = Chunk("00dc", std::vector<uint8_t>(8, 0x55));
        movi.insert(movi.end(), c.begin(), c.end());
    }
    std::vector<uint8_t> body;
    PutTag(&body, "AVI ");
    std::vector<uint8_t> h = List("hdrl", hdrl);
    body.insert(body.end(), h.begin(), h.end());
    std::vector<uint8_t> m = List("movi", movi);
    body.insert(body.end(), m.begin(), m.end());
    std::vector<uint8_t> ix = Chunk("idx1", idx1);
    body.insert(body.end(), ix.begin(), ix.end());
    std::vector<uint8_t> file;
    PutTag(&file, "RIFF");
    Put32(&file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

void TestAac() {
    a11::AacConfig c;
    // AAC-LC, 44.1 kHz, stereo: 00010 0100 0010 0...
    const uint8_t lc[] = {0x12, 0x10};
    CHECK(a11::ParseAacConfig(lc, sizeof(lc), &c) && c.sampleRate == 44100 && c.channels == 2);
    // Explicit frequency: aot 2, index 15, 24-bit 37800 Hz, channel config 2.
    // bits: 00010 1111 000000001001001110101000 0010 -> 0x17 0x80 0x49 0xD4 0x10
    const uint8_t explicitFreq[] = {0x17, 0x80, 0x49, 0xD4, 0x10};
    CHECK(a11::ParseAacConfig(explicitFreq, sizeof(explicitFreq), &c));
    CHECK(c.sampleRate == 37800 && c.channels == 2);
    // Channel configuration 7 is eight channels, 11 seven, 12 and 14 eight.
    const struct { uint8_t cfg; int channels; } map[] = {{1, 1}, {6, 6}, {7, 8}, {11, 7}, {12, 8}, {14, 8}};
    for (const auto &m : map) {
        uint8_t b[2] = {0x12, static_cast<uint8_t>(0x10 | (m.cfg << 3))};
        // aot 2, sfi 4 (0100), cfg in the next four bits.
        b[0] = 0x12;
        b[1] = static_cast<uint8_t>(0x00 | (m.cfg << 3));
        CHECK(a11::ParseAacConfig(b, 2, &c) && c.channels == m.channels);
    }
    uint8_t zero[2] = {0x12, 0x00};
    CHECK(!a11::ParseAacConfig(zero, 2, &c));  // channel configuration 0 (PCE)
    uint8_t reservedRate[2] = {0x17, 0x80 - 0x80 + 0x68};  // sfi 13
    CHECK(!a11::ParseAacConfig(reservedRate, 1, &c));
    // HE-AAC: object type 5, core 22050 Hz stereo, extension rate 44100 Hz.
    const uint8_t heAac[] = {0x2b, 0x92, 0x08};
    CHECK(a11::ParseAacConfig(heAac, sizeof(heAac), &c) && c.objectType == 5);
    CHECK(c.sampleRate == 44100 && c.channels == 2);
}

void TestMp3() {
    a11::Mp3Header h;
    const uint8_t mpeg1[] = {0xFF, 0xFB, 0x90, 0x64};  // 128 kbps, 44.1 kHz, no padding, joint stereo
    CHECK(a11::ParseMp3Header(mpeg1, 4, &h) && h.frameBytes == 417 && h.samplesPerFrame == 1152);
    const uint8_t mpeg25_8k[] = {0xFF, 0xE3, 0x28, 0xC4};  // MPEG-2.5, layer III, 8 kHz, mono
    CHECK(a11::ParseMp3Header(mpeg25_8k, 4, &h) && h.sampleRate == 8000 && h.channels == 1);
    const uint8_t bad[] = {0xFF, 0xFB, 0xF0, 0x64};
    CHECK(!a11::ParseMp3Header(bad, 4, &h));
}


std::vector<uint8_t> Mp3Frame(uint8_t payload = 0x55) {
    std::vector<uint8_t> frame(417, payload);
    const uint8_t header[] = {0xFF, 0xFB, 0x90, 0x64};
    std::copy(header, header + sizeof(header), frame.begin());
    return frame;
}

class RepeatingMp3Source : public a11::ByteSource {
public:
    explicit RepeatingMp3Source(size_t frames) : mSize(frames * 417) {}

    ssize_t readAt(int64_t offset, void *data, size_t bytes) override {
        if (offset < 0 || static_cast<uint64_t>(offset) >= mSize) {
            return 0;
        }
        const size_t count = std::min(bytes, mSize - static_cast<size_t>(offset));
        auto *output = static_cast<uint8_t *>(data);
        const uint8_t header[] = {0xFF, 0xFB, 0x90, 0x64};
        for (size_t index = 0; index < count; ++index) {
            const size_t inFrame = (static_cast<size_t>(offset) + index) % 417;
            output[index] = inFrame < 4 ? header[inFrame] : 0x55;
        }
        maxReadBytes = std::max(maxReadBytes, bytes);
        totalReadBytes += count;
        ++readCalls;
        return static_cast<ssize_t>(count);
    }

    int64_t size() override { return static_cast<int64_t>(mSize); }
    size_t maxReadBytes = 0;
    size_t totalReadBytes = 0;
    size_t readCalls = 0;

private:
    size_t mSize;
};

void TestMp3LargeChunk() {
    // A valid 16,680,000-byte chunk contains 40,000 complete MPEG-1 frames.
    // Returning its first frame must leave the rest in the byte source.
    constexpr size_t kFrames = 40000;
    RepeatingMp3Source source(kFrames);
    a11::TrackInfo track;
    track.transform = a11::Transform::kMp3Frames;
    track.packets.push_back({0, 16680000, 123000, true});
    a11::TrackReader reader(&source, &track);
    a11::Sample sample;
#if defined(A11_CONTAINER_ALLOCATION_PROBE)
    const size_t initialBytes = allocation_probe::liveBytes;
    allocation_probe::peakBytes = initialBytes;
#endif
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kOk);
#if defined(A11_CONTAINER_ALLOCATION_PROBE)
    const size_t peakBytes = allocation_probe::peakBytes - initialBytes;
    const size_t retainedBytes = allocation_probe::liveBytes - initialBytes;
    printf("MP3 first sample: reader_bytes=%zu peak_heap=%zu retained_heap=%zu "
           "source_bytes=%zu max_read=%zu\n", sizeof(reader), peakBytes, retainedBytes,
            source.totalReadBytes, source.maxReadBytes);
    CHECK(sizeof(reader) + peakBytes <= 65536);
    CHECK(sizeof(reader) + retainedBytes <= 65536);
#endif
    CHECK(source.totalReadBytes <= 16384);
    CHECK(source.maxReadBytes <= 16384);
    const std::vector<uint8_t> expected = Mp3Frame();
    CHECK(sample.data == expected && sample.timeUs == 123000 && sample.key);
    for (size_t frame = 1; frame < kFrames; ++frame) {
        const auto status = reader.next(&sample);
        CHECK(status == a11::TrackReader::Status::kOk);
        if (status != a11::TrackReader::Status::kOk) {
            return;
        }
        CHECK(sample.data == expected && sample.key);
        CHECK(sample.timeUs == 123000 + static_cast<int64_t>(frame * 1152 * 1000000 / 44100));
    }
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kEndOfStream);
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kEndOfStream);
    CHECK(source.totalReadBytes == 16680000);
    // Batching source reads avoids a separate DataSource call for each frame.
    CHECK(source.readCalls < kFrames / 4);
    printf("MP3 complete chunk: frames=%zu source_bytes=%zu read_calls=%zu\n",
            kFrames, source.totalReadBytes, source.readCalls);
}

void TestMp3ChunkBoundaries() {
    std::vector<uint8_t> data;
    for (uint8_t frame = 0; frame < 4; ++frame) {
        const auto bytes = Mp3Frame(static_cast<uint8_t>(0x50 + frame));
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    // An incomplete header at EOS never becomes a decoder sample.
    data.insert(data.end(), {0xFF, 0xFB});
    MemorySource source(std::move(data));
    a11::TrackInfo track;
    track.transform = a11::Transform::kMp3Frames;
    track.packets = {{0, 2, 100000, true}, {2, 98, 500000, true},
                    {100, 500, 800000, true}, {600, 234, 900000, true},
                    {834, 0, 1500000, true}, {834, 834, 2000000, true},
                    {1668, 2, 3000000, true}};
    a11::TrackReader reader(&source, &track);
    a11::Sample sample;
    const int64_t expectedTimes[] = {100000, 126122, 2000000, 2026122};
    for (size_t frame = 0; frame < 4; ++frame) {
        CHECK(reader.next(&sample) == a11::TrackReader::Status::kOk);
        CHECK(sample.data == Mp3Frame(static_cast<uint8_t>(0x50 + frame)));
        CHECK(sample.timeUs == expectedTimes[frame] && sample.key);
    }
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kEndOfStream);
    int64_t target;
    CHECK(reader.seek(0, a11::SeekMode::kPreviousSync, &target));
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kOk);
    CHECK(sample.data == Mp3Frame(0x50) && sample.timeUs == 100000);
    // Seek while the read window contains the beginning of the following frame.
    CHECK(reader.seek(2000000, a11::SeekMode::kClosest, &target) && target == 2000000);
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kOk);
    CHECK(sample.data == Mp3Frame(0x52) && sample.timeUs == 2000000);
    CHECK(!reader.seek(4000000, a11::SeekMode::kNextSync, &target));
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kEndOfStream);
}

void TestMp3Resynchronization() {
    // Junk crosses the read window; the largest supported frame crosses chunks.
    std::vector<uint8_t> data(20000, 0);
    std::vector<uint8_t> frame(1441, 0x55);
    const uint8_t header[] = {0xFF, 0xE3, 0xEA, 0xC4};
    std::copy(header, header + 4, frame.begin());
    data.insert(data.end(), frame.begin(), frame.end());
    MemorySource source(std::move(data));
    a11::TrackInfo track;
    track.transform = a11::Transform::kMp3Frames;
    track.packets = {{0, 20100, 77000, true}, {20100, 1341, 88000, true}};
    a11::TrackReader reader(&source, &track);
    a11::Sample sample;
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kOk);
    CHECK(sample.data == frame && sample.timeUs == 77000);
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kEndOfStream);
}

void TestMp3Errors() {
    MemorySource source(Mp3Frame());
    a11::TrackInfo track;
    track.transform = a11::Transform::kMp3Frames;
    track.packets = {{0, 417, 0, true}, {417, 417, 26122, true}};
    a11::TrackReader reader(&source, &track);
    a11::Sample sample;
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kOk);
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kIoError);
    int64_t target;
    CHECK(reader.seek(0, a11::SeekMode::kPreviousSync, &target));
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kOk);
    track.packets = {{0, a11::kMaxPacketBytes + 1, 0, true}};
    CHECK(reader.seek(0, a11::SeekMode::kPreviousSync, &target));
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kMalformed);
    track.packets = {{UINT64_MAX, 417, 0, true}};
    CHECK(reader.seek(0, a11::SeekMode::kPreviousSync, &target));
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kIoError);
}


void TestMp3TimestampLimit() {
    RepeatingMp3Source source(2);
    a11::TrackInfo track;
    track.transform = a11::Transform::kMp3Frames;
    track.packets = {{0, 834, INT64_MAX, true}};
    a11::TrackReader reader(&source, &track);
    a11::Sample sample;
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kOk);
    CHECK(sample.timeUs == INT64_MAX);
    CHECK(reader.next(&sample) == a11::TrackReader::Status::kMalformed);
}

void TestUnits() {
    int64_t us;
    CHECK(a11::UnitsToUs(25, 1, 25, &us) && us == 1000000);
    CHECK(!a11::UnitsToUs(UINT64_MAX / 2, UINT32_MAX, 1, &us));
    CHECK(!a11::UnitsToUs(1, 1, 0, &us));
}

void TestAvc() {
    std::vector<uint8_t> out;
    const uint8_t ok[] = {0, 0, 0, 2, 0x65, 0x88, 0, 0, 0, 1, 0x41};
    CHECK(a11::AvcToAnnexB(ok, sizeof(ok), 4, &out) && out.size() == 4 + 2 + 4 + 1);
    const uint8_t overrun[] = {0, 0, 0, 9, 0x65};
    CHECK(!a11::AvcToAnnexB(overrun, sizeof(overrun), 4, &out));
    const uint8_t twoByte[] = {0, 1, 0x41, 0, 2, 0x65, 0x88};
    CHECK(a11::AvcToAnnexB(twoByte, sizeof(twoByte), 2, &out) && out.size() == 4 + 1 + 4 + 2);
    // One-byte prefixes over one-byte NAL units grow by 5/2; the bound covers it.
    std::vector<uint8_t> tiny;
    for (int i = 0; i < 50; ++i) {
        tiny.push_back(1);
        tiny.push_back(0x09);
    }
    CHECK(a11::AvcToAnnexB(tiny.data(), tiny.size(), 1, &out) && out.size() == 250);
    CHECK(a11::AnnexBMaxSize(tiny.size(), 1) >= out.size());
    CHECK(a11::AnnexBMaxSize(100, 4) == 125);
}


void TestAvcOutputLimit() {
    // Short lengths and one-byte NALs expand toward the 16 MiB decoder-input
    // ceiling. A final two-byte NAL reaches the ceiling exactly.
    constexpr size_t kTinyNals = 3355442;
    for (int prefix = 1; prefix <= 3; ++prefix) {
        const size_t storedNalBytes = static_cast<size_t>(prefix) + 1;
        std::vector<uint8_t> packet(kTinyNals * storedNalBytes, 0);
        for (size_t index = 0; index < kTinyNals; ++index) {
            packet[index * storedNalBytes + prefix - 1] = 1;
            packet[index * storedNalBytes + prefix] = 0x09;
        }
        packet.insert(packet.end(), static_cast<size_t>(prefix) - 1, 0);
        packet.insert(packet.end(), {2, 0x65, 0x88});
        {
            std::vector<uint8_t> output;
            CHECK(a11::AvcToAnnexB(packet.data(), packet.size(), prefix, &output));
            CHECK(output.size() == 16777216);
            if (output.size() == 16777216) {
                CHECK(output[0] == 0 && output[3] == 1 && output[4] == 0x09);
                CHECK(output[output.size() - 2] == 0x65 && output.back() == 0x88);
            }
        }
        packet.insert(packet.end(), static_cast<size_t>(prefix) - 1, 0);
        packet.insert(packet.end(), {1, 0x09});
        {
            std::vector<uint8_t> output;
#if defined(A11_CONTAINER_ALLOCATION_PROBE)
            const size_t initialBytes = allocation_probe::liveBytes;
            allocation_probe::peakBytes = initialBytes;
#endif
            CHECK(!a11::AvcToAnnexB(packet.data(), packet.size(), prefix, &output));
            CHECK(output.empty());
#if defined(A11_CONTAINER_ALLOCATION_PROBE)
            const size_t peakBytes = allocation_probe::peakBytes - initialBytes;
            printf("AVC rejected expansion: prefix=%d peak_heap=%zu output_bytes=%zu\n",
                    prefix, peakBytes, output.size());
            CHECK(peakBytes <= 65536);
#endif
        }
        CHECK(a11::AnnexBMaxSize(a11::kMaxPacketBytes, prefix) == a11::kMaxPacketBytes);
        CHECK(a11::AnnexBMaxSize(SIZE_MAX, prefix) == a11::kMaxPacketBytes);
        a11::TrackInfo track;
        track.transform = a11::Transform::kAvcAnnexB;
        track.nalLengthSize = prefix;
        track.packets = {{0, static_cast<uint32_t>(packet.size()), 0, true}};
        MemorySource source(std::move(packet));
        a11::TrackReader reader(&source, &track);
        a11::Sample sample;
        CHECK(reader.next(&sample) == a11::TrackReader::Status::kMalformed);
    }
    // Long NALs fit even when the one-byte-NAL worst-case ratio exceeds 16 MiB.
    std::vector<uint8_t> packet(12582912, 0x41);
    for (size_t position = 0; position < packet.size(); position += 256) {
        packet[position] = 255;
    }
    std::vector<uint8_t> output;
    CHECK(a11::AvcToAnnexB(packet.data(), packet.size(), 1, &output));
    CHECK(output.size() == 12730368);
}

void TestAvi() {
    a11::Container c;
    {
        MemorySource ok(MakeAvi(1, 25, 0, 48, 4));
        CHECK(a11::ParseAvi(&ok, &c) && c.tracks.size() == 1 && c.tracks[0].packets.size() == 4);
        CHECK(c.tracks[0].packets[3].timeUs == 120000);
    }
    {
        // dwStart shifts the timeline by start * scale / rate.
        MemorySource shifted(MakeAvi(1, 25, 50, 48, 4));
        CHECK(a11::ParseAvi(&shifted, &c) && c.tracks[0].packets[0].timeUs == 2000000);
    }
    {
        MemorySource minHeight(MakeAvi(1, 25, 0, INT32_MIN, 4));
        CHECK(!a11::ParseAvi(&minHeight, &c));
    }
    {
        // The timestamp of packet 1 needs scale * 1e6 > 2^64.
        MemorySource huge(MakeAvi(UINT32_MAX, 1, 0xFFFFFFFFu, 48, 4));
        CHECK(!a11::ParseAvi(&huge, &c));
    }
    {
        // Negative height (top-down bitmap) is valid.
        MemorySource topDown(MakeAvi(1, 25, 0, -48, 2));
        CHECK(a11::ParseAvi(&topDown, &c) && c.tracks[0].height == 48);
    }
}

}  // namespace

int main() {
    TestAac();
    TestMp3();
    TestMp3LargeChunk();
    TestMp3ChunkBoundaries();
    TestMp3Resynchronization();
    TestMp3Errors();
    TestMp3TimestampLimit();
    TestUnits();
    TestAvc();
    TestAvcOutputLimit();
    TestAvi();
    if (gFailures == 0) {
        printf("unit tests ok\n");
    }
    return gFailures == 0 ? 0 : 1;
}
