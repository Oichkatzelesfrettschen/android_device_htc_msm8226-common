/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Codec2 software decoder for On2 VP6 and VP6A video (FLV).
 */

#ifndef ANDROID_C2_SOFT_A11_VP6_DEC_H_
#define ANDROID_C2_SOFT_A11_VP6_DEC_H_

#include <SimpleC2Component.h>

#include "a11vp6.h"

namespace android {

struct C2SoftA11Vp6Dec : public SimpleC2Component {
    class IntfImpl;

    C2SoftA11Vp6Dec(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    ~C2SoftA11Vp6Dec() override;

    // From SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> &pool) override;

private:
    // csd-0: the FLV crop byte, followed by 0x01 for the alpha variant.
    bool openDecoder();
    void closeDecoder();
    void finishWork(const std::unique_ptr<C2Work> &work, const std::shared_ptr<C2GraphicBlock> &block);

    std::shared_ptr<IntfImpl> mIntf;
    A11Vp6 *mDecoder = nullptr;
    uint8_t mCsd[2] = {0, 0};
    size_t mCsdSize = 0;
    uint32_t mWidth = 320;
    uint32_t mHeight = 240;
    bool mSignalledError = false;
    bool mSignalledOutputEos = false;

    C2_DO_NOT_COPY(C2SoftA11Vp6Dec);
};

}  // namespace android

#endif  // ANDROID_C2_SOFT_A11_VP6_DEC_H_
