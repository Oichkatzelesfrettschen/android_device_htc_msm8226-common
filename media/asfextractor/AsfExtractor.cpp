/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MediaExtractor plugin for ASF files carrying WMA, VC-1 and MPEG-4 streams.
 */

#define LOG_TAG "AsfExtractor"

#include <inttypes.h>
#include <string.h>

#include <memory>
#include <vector>

#include <media/MediaExtractorPluginApi.h>
#include <media/MediaExtractorPluginHelper.h>
#include <media/NdkMediaFormat.h>
#include <utils/Log.h>

#include "AsfParser.h"
#include "AsfTracks.h"

namespace android {

namespace {

void FillTrackFormat(const asf::TrackFormat &format, AMediaFormat *meta) {
    AMediaFormat_clear(meta);
    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, format.mime.c_str());
    if (!format.csd0.empty()) {
        AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_CSD_0, format.csd0.data(),
                format.csd0.size());
    }
    if (format.audio) {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_CHANNEL_COUNT, format.channels);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_SAMPLE_RATE, format.sampleRate);
    } else {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_WIDTH, format.width);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_HEIGHT, format.height);
        if (format.frameRate > 0) {
            AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_FRAME_RATE, format.frameRate);
        }
    }
    if (format.bitrate > 0) {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_BIT_RATE, format.bitrate);
    }
    if (format.durationUs > 0) {
        AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, format.durationUs);
    }
    AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, (int32_t)format.maxInputSize);
}

class HelperByteSource : public asf::ByteSource {
public:
    explicit HelperByteSource(DataSourceHelper *source) : mSource(source) {}
    ssize_t readAt(int64_t offset, void *data, size_t size) override {
        return mSource->readAt(offset, data, size);
    }

private:
    DataSourceHelper *mSource;
};

class AsfTrack : public MediaTrackHelper {
public:
    AsfTrack(DataSourceHelper *source, const asf::AsfFile &file,
            const asf::StreamInfo &stream, const asf::TrackFormat &format)
        : mByteSource(source), mReader(file, &mByteSource, stream), mFormat(format) {}

    media_status_t start() override {
        if (!mBufferGroup->init(4 /* buffers */, mFormat.maxInputSize, 16 /* growth limit */)) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        return AMEDIA_OK;
    }

    media_status_t stop() override { return AMEDIA_OK; }

    media_status_t getFormat(AMediaFormat *meta) override {
        FillTrackFormat(mFormat, meta);
        return AMEDIA_OK;
    }

    media_status_t read(MediaBufferHelper **out, const ReadOptions *options) override {
        *out = nullptr;
        int64_t seekTimeUs;
        ReadOptions::SeekMode mode;
        if (options != nullptr && options->getSeekTo(&seekTimeUs, &mode)) {
            mReader.seek(seekTimeUs);
        }

        asf::MediaObject object;
        if (!mReader.next(&object)) {
            return AMEDIA_ERROR_END_OF_STREAM;
        }
        std::vector<uint8_t> sample = asf::PrepareSample(mFormat, std::move(object.data));
        MediaBufferHelper *buffer = nullptr;
        status_t err = mBufferGroup->acquire_buffer(&buffer, false /* nonBlocking */,
                sample.size());
        if (err != OK || buffer == nullptr) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        if (buffer->size() < sample.size()) {
            ALOGE("sample of %zu bytes exceeds buffer of %zu", sample.size(), buffer->size());
            buffer->release();
            return AMEDIA_ERROR_MALFORMED;
        }
        memcpy(buffer->data(), sample.data(), sample.size());
        buffer->set_range(0, sample.size());
        AMediaFormat *meta = buffer->meta_data();
        AMediaFormat_clear(meta);
        AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_TIME_US, object.timeUs);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_IS_SYNC_FRAME, object.keyFrame ? 1 : 0);
        *out = buffer;
        return AMEDIA_OK;
    }

private:
    HelperByteSource mByteSource;
    asf::StreamReader mReader;
    asf::TrackFormat mFormat;
};

class AsfExtractor : public MediaExtractorPluginHelper {
public:
    explicit AsfExtractor(DataSourceHelper *source) : mSource(source), mByteSource(source) {
        off64_t size = 0;
        if (mSource->getSize(&size) != OK) {
            size = 0;
        }
        mValid = mFile.parse(&mByteSource, size);
        if (!mValid) {
            return;
        }
        for (const asf::StreamInfo &stream : mFile.streams()) {
            asf::TrackFormat format;
            if (asf::DescribeStream(mFile, stream, &format)) {
                mTracks.push_back({&stream, format});
            } else {
                ALOGI("stream %d (kind %d, tag 0x%x, fourcc 0x%08x) has no decoder path",
                        stream.number, (int)stream.kind, stream.formatTag, stream.fourcc);
            }
        }
    }

    ~AsfExtractor() override { delete mSource; }

    size_t countTracks() override { return mValid ? mTracks.size() : 0; }

    MediaTrackHelper *getTrack(size_t index) override {
        if (index >= countTracks()) {
            return nullptr;
        }
        return new AsfTrack(mSource, mFile, *mTracks[index].stream, mTracks[index].format);
    }

    media_status_t getTrackMetaData(AMediaFormat *meta, size_t index, uint32_t) override {
        if (index >= countTracks()) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        FillTrackFormat(mTracks[index].format, meta);
        return AMEDIA_OK;
    }

    media_status_t getMetaData(AMediaFormat *meta) override {
        AMediaFormat_clear(meta);
        bool video = false;
        for (const Track &t : mTracks) {
            video |= t.stream->kind == asf::StreamKind::kVideo;
        }
        AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME,
                video ? "video/x-ms-asf" : "audio/x-ms-wma");
        if (mFile.durationUs() > 0) {
            AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, mFile.durationUs());
        }
        return AMEDIA_OK;
    }

    const char *name() override { return "AsfExtractor"; }

private:
    struct Track {
        const asf::StreamInfo *stream;
        asf::TrackFormat format;
    };

    DataSourceHelper *mSource;
    HelperByteSource mByteSource;
    asf::AsfFile mFile;
    std::vector<Track> mTracks;
    bool mValid = false;
};

// ASF Header Object GUID 75B22630-668E-11CF-A6D9-00AA0062CE6C in file order.
constexpr uint8_t kAsfHeaderGuid[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                        0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};

bool SniffAsf(DataSourceHelper *source, float *confidence) {
    uint8_t header[16];
    if (source->readAt(0, header, sizeof(header)) != (ssize_t)sizeof(header)
            || memcmp(header, kAsfHeaderGuid, sizeof(header)) != 0) {
        return false;
    }
    *confidence = 0.5f;
    return true;
}

const char *kExtensions[] = {
    "asf",
    "wma",
    "wmv",
    nullptr,
};

}  // namespace

extern "C" {

__attribute__((visibility("default")))
ExtractorDef GETEXTRACTORDEF() {
    return {
        EXTRACTORDEF_VERSION,
        UUID("5b0f3d0e-8a5b-4f0c-9a41-a11c0de0a5f1"),
        1,
        "ASF Extractor",
        {
            .v3 = {
                [](CDataSource *source, float *confidence, void **,
                        FreeMetaFunc *) -> CreatorFunc {
                    DataSourceHelper helper(source);
                    if (!SniffAsf(&helper, confidence)) {
                        return nullptr;
                    }
                    return [](CDataSource *source, void *) -> CMediaExtractor * {
                        return wrap(new AsfExtractor(new DataSourceHelper(source)));
                    };
                },
                kExtensions,
            },
        },
    };
}

}  // extern "C"

}  // namespace android
