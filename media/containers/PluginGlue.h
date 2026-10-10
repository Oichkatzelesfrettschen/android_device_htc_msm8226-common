/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MediaExtractor plugin glue over the container tables in Tracks.h.
 */

#pragma once

#include <string.h>

#include <algorithm>

#include <media/MediaExtractorPluginApi.h>
#include <media/MediaExtractorPluginHelper.h>
#include <media/NdkMediaFormat.h>

#include "Tracks.h"

namespace android {

class DataSourceBytes : public a11::ByteSource {
public:
    explicit DataSourceBytes(DataSourceHelper *source) : mSource(source) {}
    ssize_t readAt(int64_t offset, void *data, size_t size) override {
        return mSource->readAt(static_cast<off64_t>(offset), data, size);
    }
    int64_t size() override {
        off64_t size = 0;
        return mSource->getSize(&size) == OK ? static_cast<int64_t>(size) : -1;
    }

private:
    DataSourceHelper *mSource;
};

inline void FillTrackFormat(const a11::TrackInfo &t, AMediaFormat *meta) {
    AMediaFormat_clear(meta);
    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, t.mime.c_str());
    if (!t.csd0.empty()) {
        AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_CSD_0, t.csd0.data(), t.csd0.size());
    }
    if (!t.csd1.empty()) {
        AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_CSD_1, t.csd1.data(), t.csd1.size());
    }
    if (t.video) {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_WIDTH, t.width);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_HEIGHT, t.height);
        if (t.frameRate > 0) {
            AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_FRAME_RATE, t.frameRate);
        }
    } else {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_CHANNEL_COUNT, t.channels);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_SAMPLE_RATE, t.sampleRate);
        if (t.pcmEncoding != 0) {
            AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_PCM_ENCODING, t.pcmEncoding);
        }
    }
    if (t.bitrate > 0) {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_BIT_RATE, t.bitrate);
    }
    if (t.durationUs > 0) {
        AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, t.durationUs);
    }
    AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
            static_cast<int32_t>(std::min<size_t>(t.maxInput, a11::kMaxPacketBytes)));
}

class ContainerTrack : public MediaTrackHelper {
public:
    ContainerTrack(DataSourceHelper *source, const a11::TrackInfo *track)
        : mBytes(source), mTrack(track), mReader(&mBytes, track) {}

    media_status_t start() override {
        // A small pool; acquire_buffer grows a buffer to the sample size, so
        // one oversized packet never multiplies by the pool size.
        constexpr size_t kInitialBytes = 256u << 10;
        if (!mBufferGroup->init(2, std::min<size_t>(mTrack->maxInput, kInitialBytes), 4)) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        int64_t ignored;
        mReader.seek(0, a11::SeekMode::kPreviousSync, &ignored);
        return AMEDIA_OK;
    }

    media_status_t stop() override { return AMEDIA_OK; }

    media_status_t getFormat(AMediaFormat *meta) override {
        FillTrackFormat(*mTrack, meta);
        return AMEDIA_OK;
    }

    media_status_t read(MediaBufferHelper **out, const ReadOptions *options) override {
        *out = nullptr;
        int64_t seekValue;
        ReadOptions::SeekMode mode;
        int64_t targetUs = -1;
        if (options != nullptr && options->getSeekTo(&seekValue, &mode)) {
            a11::SeekMode m;
            switch (mode) {
                case ReadOptions::SEEK_NEXT_SYNC: m = a11::SeekMode::kNextSync; break;
                case ReadOptions::SEEK_CLOSEST_SYNC: m = a11::SeekMode::kClosestSync; break;
                case ReadOptions::SEEK_CLOSEST: m = a11::SeekMode::kClosest; break;
                case ReadOptions::SEEK_FRAME_INDEX: m = a11::SeekMode::kFrameIndex; break;
                default: m = a11::SeekMode::kPreviousSync; break;
            }
            mReader.seek(seekValue, m, &targetUs);
        }
        a11::Sample sample;
        switch (mReader.next(&sample)) {
            case a11::TrackReader::Status::kOk: break;
            case a11::TrackReader::Status::kEndOfStream: return AMEDIA_ERROR_END_OF_STREAM;
            case a11::TrackReader::Status::kIoError: return AMEDIA_ERROR_IO;
            default: return AMEDIA_ERROR_MALFORMED;
        }
        MediaBufferHelper *buffer = nullptr;
        status_t err = mBufferGroup->acquire_buffer(&buffer, false /* nonBlocking */, sample.data.size());
        if (err != OK || buffer == nullptr) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        if (buffer->size() < sample.data.size()) {
            buffer->release();
            return AMEDIA_ERROR_MALFORMED;
        }
        memcpy(buffer->data(), sample.data.data(), sample.data.size());
        buffer->set_range(0, sample.data.size());
        AMediaFormat *meta = buffer->meta_data();
        AMediaFormat_clear(meta);
        AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_TIME_US, sample.timeUs);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_IS_SYNC_FRAME, sample.key ? 1 : 0);
        if (targetUs >= 0) {
            AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_TARGET_TIME, targetUs);
        }
        *out = buffer;
        return AMEDIA_OK;
    }

private:
    DataSourceBytes mBytes;
    const a11::TrackInfo *mTrack;
    a11::TrackReader mReader;
};

using ParseFn = bool (*)(a11::ByteSource *, a11::Container *);

class ContainerExtractor : public MediaExtractorPluginHelper {
public:
    ContainerExtractor(DataSourceHelper *source, ParseFn parse, const char *name,
            const char *containerMime)
        : mSource(source), mName(name), mContainerMime(containerMime) {
        DataSourceBytes bytes(source);
        mValid = parse(&bytes, &mContainer);
    }

    ~ContainerExtractor() override { delete mSource; }

    size_t countTracks() override { return mValid ? mContainer.tracks.size() : 0; }

    MediaTrackHelper *getTrack(size_t index) override {
        if (index >= countTracks()) {
            return nullptr;
        }
        return new ContainerTrack(mSource, &mContainer.tracks[index]);
    }

    media_status_t getTrackMetaData(AMediaFormat *meta, size_t index, uint32_t) override {
        if (index >= countTracks()) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        FillTrackFormat(mContainer.tracks[index], meta);
        return AMEDIA_OK;
    }

    media_status_t getMetaData(AMediaFormat *meta) override {
        AMediaFormat_clear(meta);
        AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, mContainerMime);
        if (mContainer.durationUs > 0) {
            AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, mContainer.durationUs);
        }
        return AMEDIA_OK;
    }

    const char *name() override { return mName; }

private:
    DataSourceHelper *mSource;
    const char *mName;
    const char *mContainerMime;
    a11::Container mContainer;
    bool mValid = false;
};

}  // namespace android
