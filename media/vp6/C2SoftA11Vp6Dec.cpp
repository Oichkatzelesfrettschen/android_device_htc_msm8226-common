/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Codec2 software decoder for On2 VP6 and VP6A video (FLV). The decoder core
 * is the FFmpeg VP6 decoder (LGPL-2.1-or-later) vendored under third_party.
 */

#define LOG_TAG "C2SoftA11Vp6Dec"
#include <log/log.h>

#include <algorithm>

#include <media/stagefright/foundation/AUtils.h>

#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <Codec2BufferUtils.h>
#include <SimpleC2Interface.h>

#include "C2SoftA11Vp6Dec.h"

namespace android {

namespace {

constexpr char kComponentName[] = "c2.a11.vp6.decoder";
constexpr char kMimeVp6[] = "video/x-vnd.on2.vp6";
constexpr size_t kMinInputBufferSize = 512 * 1024;
constexpr uint32_t kMaxDimension = 2048;

void fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    uint32_t flags = 0;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
    }
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(flags);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

}  // namespace

class C2SoftA11Vp6Dec::IntfImpl : public SimpleInterface<void>::BaseParams {
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : SimpleInterface<void>::BaseParams(helper, kComponentName, C2Component::KIND_DECODER,
                  C2Component::DOMAIN_VIDEO, kMimeVp6) {
        noPrivateBuffers();
        noInputReferences();
        noOutputReferences();
        noInputLatency();
        noTimeStretch();

        // VP6 has no B frames: output follows input one to one.
        addParameter(
                DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL))
                .build());

        addParameter(
                DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
                .withFields({
                    C2F(mSize, width).inRange(2, kMaxDimension),
                    C2F(mSize, height).inRange(2, kMaxDimension),
                })
                .withSetter(SizeSetter)
                .build());

        addParameter(
                DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
                .withDefault(new C2StreamMaxPictureSizeTuning::output(0u, 320, 240))
                .withFields({
                    C2F(mMaxSize, width).inRange(2, kMaxDimension, 2),
                    C2F(mMaxSize, height).inRange(2, kMaxDimension, 2),
                })
                .withSetter(MaxPictureSizeSetter, mSize)
                .build());

        addParameter(
                DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                .withDefault(new C2StreamMaxBufferSizeInfo::input(0u, kMinInputBufferSize))
                .withFields({C2F(mMaxInputSize, value).any()})
                .calculatedAs(MaxInputSizeSetter, mMaxSize)
                .build());

        C2ChromaOffsetStruct locations[1] = {C2ChromaOffsetStruct::ITU_YUV_420_0()};
        std::shared_ptr<C2StreamColorInfo::output> defaultColorInfo =
                C2StreamColorInfo::output::AllocShared(1u, 0u, 8u /* bitDepth */, C2Color::YUV_420);
        memcpy(defaultColorInfo->m.locations, locations, sizeof(locations));
        defaultColorInfo = C2StreamColorInfo::output::AllocShared(
                {C2ChromaOffsetStruct::ITU_YUV_420_0()}, 0u, 8u /* bitDepth */, C2Color::YUV_420);
        helper->addStructDescriptors<C2ChromaOffsetStruct>();
        addParameter(
                DefineParam(mColorInfo, C2_PARAMKEY_CODED_COLOR_INFO)
                .withConstValue(defaultColorInfo)
                .build());

        addParameter(
                DefineParam(mDefaultColorAspects, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsTuning::output(
                        0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                        C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({
                    C2F(mDefaultColorAspects, range).inRange(
                            C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
                    C2F(mDefaultColorAspects, primaries).inRange(
                            C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                    C2F(mDefaultColorAspects, transfer).inRange(
                            C2Color::TRANSFER_UNSPECIFIED, C2Color::TRANSFER_OTHER),
                    C2F(mDefaultColorAspects, matrix).inRange(
                            C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)
                })
                .withSetter(DefaultColorAspectsSetter)
                .build());

        addParameter(
                DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
                .withDefault(new C2StreamPixelFormatInfo::output(0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
                .withFields({C2F(mPixelFormat, value).oneOf({HAL_PIXEL_FORMAT_YCBCR_420_888})})
                .withSetter((Setter<decltype(*mPixelFormat)>::StrictValueWithNoDeps))
                .build());
    }

    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::output> &oldMe,
            C2P<C2StreamPictureSizeInfo::output> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = oldMe.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = oldMe.v.height;
        }
        return res;
    }

    static C2R MaxPictureSizeSetter(bool mayBlock, C2P<C2StreamMaxPictureSizeTuning::output> &me,
            const C2P<C2StreamPictureSizeInfo::output> &size) {
        (void)mayBlock;
        me.set().width = c2_min(c2_max(me.v.width, size.v.width), kMaxDimension);
        me.set().height = c2_min(c2_max(me.v.height, size.v.height), kMaxDimension);
        return C2R::Ok();
    }

    static C2R MaxInputSizeSetter(bool mayBlock, C2P<C2StreamMaxBufferSizeInfo::input> &me,
            const C2P<C2StreamMaxPictureSizeTuning::output> &maxSize) {
        (void)mayBlock;
        // A key frame stays below the 4:2:0 picture size.
        me.set().value = c2_max(static_cast<size_t>(maxSize.v.width) * maxSize.v.height * 3 / 2,
                kMinInputBufferSize);
        return C2R::Ok();
    }

    static C2R DefaultColorAspectsSetter(bool mayBlock,
            C2P<C2StreamColorAspectsTuning::output> &me) {
        (void)mayBlock;
        if (me.v.range > C2Color::RANGE_OTHER) {
            me.set().range = C2Color::RANGE_OTHER;
        }
        if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
            me.set().primaries = C2Color::PRIMARIES_OTHER;
        }
        if (me.v.transfer > C2Color::TRANSFER_OTHER) {
            me.set().transfer = C2Color::TRANSFER_OTHER;
        }
        if (me.v.matrix > C2Color::MATRIX_OTHER) {
            me.set().matrix = C2Color::MATRIX_OTHER;
        }
        return C2R::Ok();
    }

private:
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
};

C2SoftA11Vp6Dec::C2SoftA11Vp6Dec(const char *name, c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl) {}

C2SoftA11Vp6Dec::~C2SoftA11Vp6Dec() {
    onRelease();
}

c2_status_t C2SoftA11Vp6Dec::onInit() {
    mSignalledError = false;
    mSignalledOutputEos = false;
    return C2_OK;
}

c2_status_t C2SoftA11Vp6Dec::onStop() {
    mSignalledError = false;
    mSignalledOutputEos = false;
    return C2_OK;
}

void C2SoftA11Vp6Dec::onReset() {
    (void)onStop();
    (void)onFlush_sm();
}

void C2SoftA11Vp6Dec::onRelease() {
    closeDecoder();
}

c2_status_t C2SoftA11Vp6Dec::onFlush_sm() {
    // The next picture after a seek is a key frame, which resets the references.
    a11_vp6_flush(mDecoder);
    mSignalledError = false;
    mSignalledOutputEos = false;
    return C2_OK;
}

bool C2SoftA11Vp6Dec::openDecoder() {
    if (mDecoder != nullptr) {
        return true;
    }
    const bool alpha = mCsdSize >= 2 && mCsd[1] != 0;
    mDecoder = a11_vp6_open(alpha ? 1 : 0, mCsdSize >= 1 ? mCsd : nullptr, mCsdSize >= 1 ? 1 : 0);
    return mDecoder != nullptr;
}

void C2SoftA11Vp6Dec::closeDecoder() {
    a11_vp6_close(mDecoder);
    mDecoder = nullptr;
}

void C2SoftA11Vp6Dec::finishWork(const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2GraphicBlock> &block) {
    std::shared_ptr<C2Buffer> buffer = createGraphicBuffer(block, C2Rect(mWidth, mHeight));
    uint32_t flags = 0;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
    }
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(flags);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.buffers.push_back(buffer);
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

void C2SoftA11Vp6Dec::process(const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.configUpdate.clear();
    work->worklets.front()->output.flags = work->input.flags;

    if (mSignalledError || mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    size_t inSize = 0u;
    C2ReadView rView = mDummyReadView;
    if (!work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        if (inSize && rView.error()) {
            ALOGE("read view map failed %d", rView.error());
            work->result = C2_CORRUPTED;
            return;
        }
    }
    const bool codecConfig = (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) != 0;
    const bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;

    if (codecConfig) {
        // csd-0 carries the FLV crop byte, then 0x01 when the stream has alpha.
        if (inSize >= 1 && inSize <= sizeof(mCsd)) {
            memcpy(mCsd, rView.data(), inSize);
            mCsdSize = inSize;
            closeDecoder();
        }
        fillEmptyWork(work);
        return;
    }

    if (inSize > 0) {
        if (!openDecoder()) {
            ALOGE("decoder open failed");
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return;
        }
        A11Vp6Frame frame;
        const int r = a11_vp6_decode(mDecoder, rView.data(), static_cast<int>(inSize), &frame);
        if (r > 0) {
            if (frame.width <= 0 || frame.height <= 0
                    || static_cast<uint32_t>(frame.width) > kMaxDimension
                    || static_cast<uint32_t>(frame.height) > kMaxDimension) {
                ALOGE("picture %dx%d out of range", frame.width, frame.height);
                mSignalledError = true;
                work->workletsProcessed = 1u;
                work->result = C2_CORRUPTED;
                return;
            }
            if (static_cast<uint32_t>(frame.width) != mWidth
                    || static_cast<uint32_t>(frame.height) != mHeight) {
                mWidth = static_cast<uint32_t>(frame.width);
                mHeight = static_cast<uint32_t>(frame.height);
                C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
                std::vector<std::unique_ptr<C2SettingResult>> failures;
                c2_status_t err = mIntf->config({&size}, C2_MAY_BLOCK, &failures);
                if (err != C2_OK) {
                    ALOGE("size config update failed");
                    mSignalledError = true;
                    work->workletsProcessed = 1u;
                    work->result = C2_CORRUPTED;
                    return;
                }
                work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(size));
            }
            std::shared_ptr<C2GraphicBlock> block;
            C2MemoryUsage usage = {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
            c2_status_t err = pool->fetchGraphicBlock(align(mWidth, 16), align(mHeight, 2),
                    HAL_PIXEL_FORMAT_YV12, usage, &block);
            if (err != C2_OK) {
                ALOGE("fetchGraphicBlock failed with status %d", err);
                work->result = err;
                return;
            }
            C2GraphicView wView = block->map().get();
            if (wView.error()) {
                ALOGE("graphic view map failed %d", wView.error());
                work->result = C2_CORRUPTED;
                return;
            }
            const C2PlanarLayout layout = wView.layout();
            convertYUV420Planar8ToYV12(
                    const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_Y]),
                    const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_U]),
                    const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_V]),
                    frame.data[0], frame.data[1], frame.data[2],
                    frame.linesize[0], frame.linesize[1], frame.linesize[2],
                    layout.planes[C2PlanarLayout::PLANE_Y].rowInc,
                    layout.planes[C2PlanarLayout::PLANE_U].rowInc,
                    layout.planes[C2PlanarLayout::PLANE_V].rowInc,
                    mWidth, mHeight);
            finishWork(work, block);
            if (eos) {
                mSignalledOutputEos = true;
            }
            return;
        }
        if (r < 0) {
            // A damaged packet drops its picture; the next key frame recovers.
            ALOGW("dropping undecodable packet (%d)", r);
        }
    }
    fillEmptyWork(work);
    if (eos) {
        mSignalledOutputEos = true;
    }
}

c2_status_t C2SoftA11Vp6Dec::drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> &pool) {
    (void)pool;
    if (drainMode == NO_DRAIN) {
        ALOGW("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }
    return C2_OK;  // every picture leaves with its input
}

class C2SoftA11Vp6Factory : public C2ComponentFactory {
public:
    C2SoftA11Vp6Factory()
        : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
                  GetCodec2PlatformComponentStore()->getParamReflector())) {}

    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component> *const component,
            std::function<void(C2Component *)> deleter) override {
        *component = std::shared_ptr<C2Component>(
                new C2SoftA11Vp6Dec(kComponentName, id,
                        std::make_shared<C2SoftA11Vp6Dec::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    c2_status_t createInterface(c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface> *const interface,
            std::function<void(C2ComponentInterface *)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2SoftA11Vp6Dec::IntfImpl>(kComponentName, id,
                        std::make_shared<C2SoftA11Vp6Dec::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    ~C2SoftA11Vp6Factory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

}  // namespace android

__attribute__((cfi_canonical_jump_table))
extern "C" ::C2ComponentFactory *CreateCodec2Factory() {
    return new ::android::C2SoftA11Vp6Factory();
}

__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyCodec2Factory(::C2ComponentFactory *factory) {
    delete factory;
}
