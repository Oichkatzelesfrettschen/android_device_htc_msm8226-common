/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the bitstream parsers and for AVI header edge cases that
 * ffmpeg does not generate: stream start offsets, INT32_MIN height, timestamp
 * overflow, oversized scale values.
 */

#include <stdio.h>
#include <string.h>

#include <vector>

#include "Codecs.h"
#include "Tracks.h"

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
        memcpy(data, mData.data() + offset, n);
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
    TestUnits();
    TestAvc();
    TestAvi();
    if (gFailures == 0) {
        printf("unit tests ok\n");
    }
    return gFailures == 0 ? 0 : 1;
}
