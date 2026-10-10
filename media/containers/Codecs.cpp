/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bitstream header parsers shared by the container readers.
 */

#include "Codecs.h"

#include <string.h>

namespace a11 {

namespace {

class BitReader {
public:
    BitReader(const uint8_t *data, size_t size) : mData(data), mSize(size) {}

    bool get(unsigned bits, uint32_t *value) {
        if (bits > 32 || mPos + bits > mSize * 8) {
            return false;
        }
        uint32_t v = 0;
        for (unsigned i = 0; i < bits; ++i) {
            v = (v << 1) | ((mData[mPos >> 3] >> (7 - (mPos & 7))) & 1u);
            ++mPos;
        }
        *value = v;
        return true;
    }

    bool skip(unsigned bits) {
        uint32_t ignored;
        while (bits > 0) {
            unsigned n = bits > 32 ? 32 : bits;
            if (!get(n, &ignored)) {
                return false;
            }
            bits -= n;
        }
        return true;
    }

    // Unsigned Exp-Golomb code, at most 32 bits of value.
    bool ue(uint32_t *value) {
        unsigned zeros = 0;
        uint32_t bit = 0;
        while (true) {
            if (!get(1, &bit)) {
                return false;
            }
            if (bit) {
                break;
            }
            if (++zeros > 31) {
                return false;
            }
        }
        uint32_t rest = 0;
        if (zeros > 0 && !get(zeros, &rest)) {
            return false;
        }
        *value = ((1u << zeros) - 1u) + rest;
        return true;
    }

    // Signed Exp-Golomb code.
    bool se(int32_t *value) {
        uint32_t k;
        if (!ue(&k)) {
            return false;
        }
        *value = (k & 1u) ? static_cast<int32_t>((k + 1u) >> 1) : -static_cast<int32_t>(k >> 1);
        return true;
    }

private:
    const uint8_t *mData;
    size_t mSize;
    size_t mPos = 0;
};

const int kMp3Bitrates[2][15] = {
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},  // MPEG-1
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},      // MPEG-2, 2.5
};
const int kMp3Rates[3][3] = {
    {44100, 48000, 32000},  // MPEG-1
    {22050, 24000, 16000},  // MPEG-2
    {11025, 12000, 8000},   // MPEG-2.5
};

const int32_t kAacRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                               22050, 16000, 12000, 11025, 8000, 7350};

}  // namespace

bool ParseMp3Header(const uint8_t *p, size_t size, Mp3Header *out) {
    if (size < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return false;
    }
    const unsigned version = (p[1] >> 3) & 3;  // 0: 2.5, 2: 2, 3: 1
    const unsigned layer = (p[1] >> 1) & 3;    // 1: Layer III
    const unsigned bitrateIdx = p[2] >> 4;
    const unsigned rateIdx = (p[2] >> 2) & 3;
    const unsigned padding = (p[2] >> 1) & 1;
    if (version == 1 || layer != 1 || bitrateIdx == 0 || bitrateIdx == 15 || rateIdx == 3) {
        return false;
    }
    const bool mpeg1 = version == 3;
    const int row = mpeg1 ? 0 : (version == 2 ? 1 : 2);
    out->sampleRate = kMp3Rates[row][rateIdx];
    out->bitrate = kMp3Bitrates[mpeg1 ? 0 : 1][bitrateIdx] * 1000;
    out->channels = ((p[3] >> 6) & 3) == 3 ? 1 : 2;
    out->samplesPerFrame = mpeg1 ? 1152 : 576;
    out->frameBytes = (mpeg1 ? 144u : 72u) * static_cast<uint32_t>(out->bitrate)
            / static_cast<uint32_t>(out->sampleRate) + padding;
    return out->frameBytes >= 4;
}

bool ParseAacConfig(const uint8_t *data, size_t size, AacConfig *out) {
    BitReader br(data, size);
    uint32_t aot, sfi, freq = 0, cfg;
    if (!br.get(5, &aot)) {
        return false;
    }
    if (aot == 31) {
        uint32_t ext;
        if (!br.get(6, &ext)) {
            return false;
        }
        aot = 32 + ext;
    }
    if (!br.get(4, &sfi)) {
        return false;
    }
    if (sfi == 15) {
        if (!br.get(24, &freq) || freq == 0) {
            return false;
        }
    } else if (sfi < 13) {
        freq = static_cast<uint32_t>(kAacRates[sfi]);
    } else {
        return false;
    }
    if (!br.get(4, &cfg)) {
        return false;
    }
    int32_t channels;
    switch (cfg) {
        case 1: case 2: case 3: case 4: case 5: case 6: channels = static_cast<int32_t>(cfg); break;
        case 7: channels = 8; break;
        case 11: channels = 7; break;
        case 12: case 14: channels = 8; break;
        default: return false;
    }
    if (freq > static_cast<uint32_t>(INT32_MAX)) {
        return false;
    }
    out->objectType = static_cast<int>(aot);
    out->sampleRate = static_cast<int32_t>(freq);
    out->channels = channels;
    return true;
}

namespace {

// Removes emulation prevention bytes (00 00 03 -> 00 00).
std::vector<uint8_t> Unescape(const uint8_t *data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    int zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        if (zeros >= 2 && data[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = data[i] == 0 ? zeros + 1 : 0;
        out.push_back(data[i]);
    }
    return out;
}

bool SkipScalingList(BitReader *br, unsigned count) {
    int32_t last = 8, next = 8;
    for (unsigned j = 0; j < count; ++j) {
        if (next != 0) {
            int32_t delta;
            if (!br->se(&delta)) {
                return false;
            }
            next = (last + delta + 256) & 255;
        }
        last = next == 0 ? last : next;
    }
    return true;
}

}  // namespace

bool ParseSpsDimensions(const uint8_t *nal, size_t size, int32_t *width, int32_t *height) {
    if (size < 5 || (nal[0] & 0x1F) != 7) {
        return false;
    }
    std::vector<uint8_t> rbsp = Unescape(nal + 1, size - 1);
    BitReader br(rbsp.data(), rbsp.size());
    uint32_t profile, flags, level, id, chroma = 1, tmp;
    if (!br.get(8, &profile) || !br.get(8, &flags) || !br.get(8, &level) || !br.ue(&id)) {
        return false;
    }
    switch (profile) {
        case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118:
        case 128: case 138: case 139: case 134: case 135: {
            if (!br.ue(&chroma) || chroma > 3) {
                return false;
            }
            if (chroma == 3 && !br.get(1, &tmp)) {
                return false;
            }
            uint32_t matrix;
            if (!br.ue(&tmp) || !br.ue(&tmp) || !br.get(1, &tmp) || !br.get(1, &matrix)) {
                return false;
            }
            if (matrix) {
                for (unsigned i = 0; i < (chroma != 3 ? 8u : 12u); ++i) {
                    uint32_t present;
                    if (!br.get(1, &present)) {
                        return false;
                    }
                    if (present && !SkipScalingList(&br, i < 6 ? 16 : 64)) {
                        return false;
                    }
                }
            }
            break;
        }
        default: break;
    }
    uint32_t pocType, refs, wMbs, hMaps, frameMbsOnly, crop = 0;
    if (!br.ue(&tmp) || !br.ue(&pocType)) {
        return false;
    }
    if (pocType == 0) {
        if (!br.ue(&tmp)) {
            return false;
        }
    } else if (pocType == 1) {
        int32_t s;
        uint32_t n;
        if (!br.get(1, &tmp) || !br.se(&s) || !br.se(&s) || !br.ue(&n) || n > 255) {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            if (!br.se(&s)) {
                return false;
            }
        }
    }
    if (!br.ue(&refs) || !br.get(1, &tmp) || !br.ue(&wMbs) || !br.ue(&hMaps)
            || !br.get(1, &frameMbsOnly)) {
        return false;
    }
    if (!frameMbsOnly && !br.skip(1)) {
        return false;
    }
    if (!br.skip(1) || !br.get(1, &crop)) {
        return false;
    }
    uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (crop && (!br.ue(&left) || !br.ue(&right) || !br.ue(&top) || !br.ue(&bottom))) {
        return false;
    }
    if (wMbs >= 1024 || hMaps >= 1024) {
        return false;
    }
    const int64_t unitX = chroma == 0 ? 1 : (chroma == 3 ? 1 : 2);
    const int64_t subH = chroma == 1 ? 2 : 1;
    const int64_t unitY = (chroma == 0 ? 1 : subH) * (2 - static_cast<int64_t>(frameMbsOnly));
    const int64_t w = (static_cast<int64_t>(wMbs) + 1) * 16 - unitX * (static_cast<int64_t>(left) + right);
    const int64_t h = (2 - static_cast<int64_t>(frameMbsOnly)) * (static_cast<int64_t>(hMaps) + 1) * 16
            - unitY * (static_cast<int64_t>(top) + bottom);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        return false;
    }
    *width = static_cast<int32_t>(w);
    *height = static_cast<int32_t>(h);
    return true;
}

bool ParseAvcConfig(const uint8_t *data, size_t size, AvcConfig *out) {
    if (size < 7 || data[0] != 1) {
        return false;
    }
    out->nalLengthSize = (data[4] & 3) + 1;
    if (out->nalLengthSize == 3) {
        return false;
    }
    static const uint8_t kStart[4] = {0, 0, 0, 1};
    size_t pos = 5;
    const unsigned numSps = data[pos++] & 0x1F;
    out->sps.clear();
    out->pps.clear();
    bool haveDims = false;
    for (unsigned i = 0; i < numSps; ++i) {
        if (pos + 2 > size) {
            return false;
        }
        const size_t len = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        if (len == 0 || pos + len > size) {
            return false;
        }
        if (!haveDims) {
            haveDims = ParseSpsDimensions(data + pos, len, &out->width, &out->height);
        }
        out->sps.insert(out->sps.end(), kStart, kStart + 4);
        out->sps.insert(out->sps.end(), data + pos, data + pos + len);
        pos += len;
    }
    if (pos >= size) {
        return false;
    }
    const unsigned numPps = data[pos++];
    for (unsigned i = 0; i < numPps; ++i) {
        if (pos + 2 > size) {
            return false;
        }
        const size_t len = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        if (len == 0 || pos + len > size) {
            return false;
        }
        out->pps.insert(out->pps.end(), kStart, kStart + 4);
        out->pps.insert(out->pps.end(), data + pos, data + pos + len);
        pos += len;
    }
    return haveDims && !out->sps.empty() && !out->pps.empty();
}

bool AvcToAnnexB(const uint8_t *data, size_t size, int lengthSize, std::vector<uint8_t> *out) {
    if (lengthSize < 1 || lengthSize > 4) {
        return false;
    }
    out->clear();
    out->reserve(size + size / 4 + 4);
    size_t pos = 0;
    while (pos < size) {
        if (pos + static_cast<size_t>(lengthSize) > size) {
            return false;
        }
        size_t len = 0;
        for (int i = 0; i < lengthSize; ++i) {
            len = (len << 8) | data[pos++];
        }
        if (len == 0 || len > size - pos) {
            return false;
        }
        static const uint8_t kStart[4] = {0, 0, 0, 1};
        out->insert(out->end(), kStart, kStart + 4);
        out->insert(out->end(), data + pos, data + pos + len);
        pos += len;
    }
    return true;
}

bool ParseSparkSize(const uint8_t *data, size_t size, int32_t *width, int32_t *height) {
    BitReader br(data, size);
    uint32_t start, version, tref, code, w = 0, h = 0;
    if (!br.get(17, &start) || start != 1 || !br.get(5, &version) || version > 1
            || !br.get(8, &tref) || !br.get(3, &code)) {
        return false;
    }
    switch (code) {
        case 0:
            if (!br.get(8, &w) || !br.get(8, &h)) {
                return false;
            }
            break;
        case 1:
            if (!br.get(16, &w) || !br.get(16, &h)) {
                return false;
            }
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
    *width = static_cast<int32_t>(w);
    *height = static_cast<int32_t>(h);
    return true;
}

bool ParseVp6KeyFrame(const uint8_t *data, size_t size, int hAdjust, int vAdjust,
        int32_t *width, int32_t *height) {
    // Byte 0: bit 7 clear on a key frame, bit 0 separated coefficients. Byte 1:
    // sub-version in bits 7..3, filter header in bits 2..1, interlace in bit 0.
    // A two-byte coefficient offset follows when the coefficients are separated
    // or no filter header is present; then the stored macroblock rows and columns.
    if (size < 6 || (data[0] & 0x80) != 0 || (data[1] & 1) != 0 || (data[1] >> 3) > 8) {
        return false;
    }
    size_t pos = 2;
    if ((data[0] & 1) || !(data[1] & 0x06)) {
        pos += 2;
    }
    if (pos + 2 > size) {
        return false;
    }
    const int32_t rows = data[pos];
    const int32_t cols = data[pos + 1];
    const int32_t w = cols * 16 - hAdjust;
    const int32_t h = rows * 16 - vAdjust;
    if (rows == 0 || cols == 0 || w <= 0 || h <= 0) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

}  // namespace a11
