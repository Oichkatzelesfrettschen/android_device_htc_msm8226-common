/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host harness: runs the AVI and FLV container parsers and track readers over
 * a file and prints a canonical description for comparison with ffprobe.
 *
 *   harness info FILE            one line per track, then one line per packet
 *   harness read FILE TRACK OUT  concatenates the track's samples into OUT
 *   harness seek FILE TRACK MODE VALUE
 *                                seeks, then prints the next sample
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

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

bool Parse(a11::ByteSource *src, a11::Container *c) {
    uint8_t head[4] = {0};
    if (src->readAt(0, head, 4) != 4) {
        return false;
    }
    if (memcmp(head, "RIFF", 4) == 0) {
        return a11::ParseAvi(src, c);
    }
    if (memcmp(head, "FLV", 3) == 0) {
        return a11::ParseFlv(src, c);
    }
    return false;
}

const char *Transform(a11::Transform t) {
    switch (t) {
        case a11::Transform::kAvcAnnexB: return "annexb";
        case a11::Transform::kMp3Frames: return "mp3frames";
        default: return "verbatim";
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: harness info|read|seek FILE ...\n");
        return 2;
    }
    FILE *f = fopen(argv[2], "rb");
    if (f == nullptr) {
        perror(argv[2]);
        return 2;
    }
    FileSource src(f);
    a11::Container c;
    if (!Parse(&src, &c)) {
        printf("unparsed\n");
        return 3;
    }
    const std::string cmd = argv[1];
    if (cmd == "info") {
        printf("container duration_us=%lld tracks=%zu\n", static_cast<long long>(c.durationUs),
                c.tracks.size());
        for (size_t i = 0; i < c.tracks.size(); ++i) {
            const a11::TrackInfo &t = c.tracks[i];
            size_t keys = 0;
            uint64_t bytes = 0;
            for (const a11::Packet &p : t.packets) {
                keys += p.key ? 1 : 0;
                bytes += p.size;
            }
            printf("track %zu mime=%s video=%d w=%d h=%d ch=%d rate=%d csd0=%zu csd1=%zu "
                   "transform=%s packets=%zu keys=%zu bytes=%llu\n",
                    i, t.mime.c_str(), t.video ? 1 : 0, t.width, t.height, t.channels,
                    t.sampleRate, t.csd0.size(), t.csd1.size(), Transform(t.transform),
                    t.packets.size(), keys, static_cast<unsigned long long>(bytes));
            for (const a11::Packet &p : t.packets) {
                printf("packet %zu %lld %u %d\n", i, static_cast<long long>(p.timeUs), p.size,
                        p.key ? 1 : 0);
            }
        }
        return 0;
    }
    if (argc < 4) {
        return 2;
    }
    const size_t index = static_cast<size_t>(atoi(argv[3]));
    if (index >= c.tracks.size()) {
        fprintf(stderr, "no such track\n");
        return 2;
    }
    a11::TrackReader reader(&src, &c.tracks[index]);
    int64_t target = -1;
    if (cmd == "read" && argc >= 5) {
        FILE *out = fopen(argv[4], "wb");
        if (out == nullptr) {
            return 2;
        }
        const a11::TrackInfo &t = c.tracks[index];
        if (t.mime == "video/avc") {
            // Parameter sets travel as csd, not in the samples.
            fwrite(t.csd0.data(), 1, t.csd0.size(), out);
            fwrite(t.csd1.data(), 1, t.csd1.size(), out);
        }
        size_t samples = 0;
        a11::Sample s;
        while (reader.next(&s) == a11::TrackReader::Status::kOk) {
            fwrite(s.data.data(), 1, s.data.size(), out);
            ++samples;
        }
        fclose(out);
        printf("samples=%zu\n", samples);
        return 0;
    }
    if (cmd == "seek" && argc >= 6) {
        static const struct { const char *name; a11::SeekMode mode; } kModes[] = {
            {"previous", a11::SeekMode::kPreviousSync}, {"next", a11::SeekMode::kNextSync},
            {"closest_sync", a11::SeekMode::kClosestSync}, {"closest", a11::SeekMode::kClosest},
            {"index", a11::SeekMode::kFrameIndex}};
        a11::SeekMode mode = a11::SeekMode::kPreviousSync;
        for (const auto &m : kModes) {
            if (!strcmp(m.name, argv[4])) {
                mode = m.mode;
            }
        }
        const bool ok = reader.seek(atoll(argv[5]), mode, &target);
        a11::Sample s;
        const a11::TrackReader::Status st = reader.next(&s);
        if (st == a11::TrackReader::Status::kOk) {
            printf("ok=%d time=%lld key=%d size=%zu target=%lld\n", ok ? 1 : 0,
                    static_cast<long long>(s.timeUs), s.key ? 1 : 0, s.data.size(),
                    static_cast<long long>(target));
        } else {
            printf("ok=%d eos\n", ok ? 1 : 0);
        }
        return 0;
    }
    return 2;
}
