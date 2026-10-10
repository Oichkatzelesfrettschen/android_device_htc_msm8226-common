/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Host test: demuxes an FLV with the a11 container parser, decodes the VP6
 * track with the vendored decoder and writes the pictures as raw yuv420p.
 *
 *   vp6_host_test FILE OUT.yuv
 */

#include <stdio.h>
#include <string.h>

#include "../a11vp6.h"
#include "Tracks.h"

namespace {

class FileSource : public a11::ByteSource {
public:
    explicit FileSource(FILE *f) : mFile(f) {
        fseeko(f, 0, SEEK_END);
        mSize = ftello(f);
    }
    ssize_t readAt(int64_t offset, void *data, size_t size) override {
        if (offset < 0 || fseeko(mFile, offset, SEEK_SET) != 0) {
            return -1;
        }
        return static_cast<ssize_t>(fread(data, 1, size, mFile));
    }
    int64_t size() override { return mSize; }

private:
    FILE *mFile;
    int64_t mSize = 0;
};

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (f == nullptr || out == nullptr) {
        return 2;
    }
    FileSource src(f);
    a11::Container c;
    if (!a11::ParseFlv(&src, &c)) {
        printf("unparsed\n");
        return 3;
    }
    const a11::TrackInfo *vt = nullptr;
    for (const a11::TrackInfo &t : c.tracks) {
        if (t.mime == "video/x-vnd.on2.vp6") {
            vt = &t;
        }
    }
    if (vt == nullptr) {
        printf("no vp6 track\n");
        return 3;
    }
    const bool alpha = vt->csd0.size() >= 2;  // csd-0 byte 1 marks VP6A
    A11Vp6 *dec = a11_vp6_open(alpha ? 1 : 0, vt->csd0.data(), 1);
    if (dec == nullptr) {
        printf("open failed\n");
        return 4;
    }
    a11::TrackReader reader(&src, vt);
    a11::Sample s;
    size_t frames = 0, bad = 0;
    while (reader.next(&s) == a11::TrackReader::Status::kOk) {
        A11Vp6Frame fr;
        const int r = a11_vp6_decode(dec, s.data.data(), static_cast<int>(s.data.size()), &fr);
        if (r < 0) {
            ++bad;
            continue;
        }
        if (r == 0) {
            continue;
        }
        for (int y = 0; y < fr.height; ++y) {
            fwrite(fr.data[0] + static_cast<ptrdiff_t>(y) * fr.linesize[0], 1, static_cast<size_t>(fr.width), out);
        }
        for (int p = 1; p < 3; ++p) {
            for (int y = 0; y < (fr.height + 1) / 2; ++y) {
                fwrite(fr.data[p] + static_cast<ptrdiff_t>(y) * fr.linesize[p], 1, static_cast<size_t>((fr.width + 1) / 2), out);
            }
        }
        ++frames;
    }
    a11_vp6_close(dec);
    fclose(out);
    printf("frames=%zu bad=%zu width=%d height=%d\n", frames, bad, vt->width, vt->height);
    return 0;
}
