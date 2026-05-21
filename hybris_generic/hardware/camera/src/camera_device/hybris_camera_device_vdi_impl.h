/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Hybris Camera Device VDI — implements ICameraDeviceVdi by holding a
 * DroidMediaCamera* obtained from droid_media_camera_connect().
 *
 * Per phase_n12_camera_droidmedia.md (N12.D pivot, 2026-05-21), the
 * hand-rolled HIDL ICameraDevice/ICameraDeviceSession path has been
 * replaced with the libdroidmedia C ABI.  All cross-boundary work
 * happens on the DroidMediaCamera* opaque pointer; the stream operator
 * VDI borrows the same pointer for preview / capture / frame delivery.
 *
 * Ownership: this VDI OWNS the DroidMediaCamera handle.  It calls
 * Droid::Loader::Disconnect on either Close() or destruction.
 */

#ifndef HYBRIS_CAMERA_DEVICE_VDI_IMPL_H
#define HYBRIS_CAMERA_DEVICE_VDI_IMPL_H

#include <mutex>
#include <string>
#include <vector>

#include "droidmedia/droidmediacamera.h"
#include "v1_0/icamera_device_vdi.h"
#include "v1_0/icamera_device_vdi_callback.h"

namespace OHOS::Camera::Hybris {

using OHOS::VDI::Camera::V1_0::ICameraDeviceVdi;
using OHOS::VDI::Camera::V1_0::ICameraDeviceVdiCallback;
using OHOS::VDI::Camera::V1_0::IStreamOperatorVdi;
using OHOS::VDI::Camera::V1_0::IStreamOperatorVdiCallback;
using OHOS::VDI::Camera::V1_0::VdiResultCallbackMode;

class HybrisCameraDeviceVdiImpl : public ICameraDeviceVdi {
public:
    /*
     * Constructed by HybrisCameraHostVdiImpl::OpenCamera after a
     * successful Droid::Loader::Connect(index).  Takes ownership of
     * the DroidMediaCamera* (will disconnect on Close/dtor).
     */
    HybrisCameraDeviceVdiImpl(std::string ohosCameraId,
                              DroidMediaCamera *cam,
                              const sptr<ICameraDeviceVdiCallback> &cb);
    ~HybrisCameraDeviceVdiImpl() override;

    int32_t GetStreamOperator(const sptr<IStreamOperatorVdiCallback> &callbackObj,
                              sptr<IStreamOperatorVdi> &streamOperator) override;
    int32_t UpdateSettings(const std::vector<uint8_t> &settings) override;
    int32_t SetResultMode(VdiResultCallbackMode mode) override;
    int32_t GetEnabledResults(std::vector<int32_t> &results) override;
    int32_t EnableResult(const std::vector<int32_t> &results) override;
    int32_t DisableResult(const std::vector<int32_t> &results) override;
    int32_t Close() override;

private:
    std::mutex                              mutex_;
    std::string                             ohosCameraId_;
    DroidMediaCamera                       *cam_ = nullptr;   /* owned */
    sptr<ICameraDeviceVdiCallback>          deviceCallback_;
    VdiResultCallbackMode                   resultMode_ =
        VdiResultCallbackMode::PER_FRAME;
    std::vector<int32_t>                    enabledResults_;
    std::vector<uint8_t>                    latestSettings_;
    bool                                    closed_ = false;
};

} // namespace OHOS::Camera::Hybris

#endif // HYBRIS_CAMERA_DEVICE_VDI_IMPL_H
