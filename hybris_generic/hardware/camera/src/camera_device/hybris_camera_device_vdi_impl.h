/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Hybris Camera Device VDI — implements ICameraDeviceVdi by bridging
 * to a Halium-side ICameraDevice acquired via
 * ICameraProvider::getCameraDeviceInterface_V3_x.
 *
 * Bring-up scope (N12.5.5):
 *   - Holds the resolved Halium ICameraDevice binder handle.
 *   - SetResultMode / EnableResult / DisableResult / GetEnabledResults
 *     are accepted but no-op (the result loop is wired in N12.7).
 *   - GetStreamOperator returns a stub IStreamOperatorVdi whose
 *     CreateStreams / Capture all return METHOD_NOT_SUPPORTED.  The real
 *     bridge to ICameraDevice::open + ICameraDeviceSession::configureStreams_3_4
 *     lands in N12.6 / N12.7.
 *   - UpdateSettings stores the latest blob.
 *   - Close releases the Halium binder handle.
 *
 * The HDF camera_host_service treats METHOD_NOT_SUPPORTED on stream
 * methods as a fatal-for-this-camera condition, so opening the camera
 * from the HAP will fail until N12.6 lands.  Reaching this state is
 * still meaningful: it proves the host-→device→stream-operator dispatch
 * chain works through our VDI, which is the precondition for streaming.
 */

#ifndef HYBRIS_CAMERA_DEVICE_VDI_IMPL_H
#define HYBRIS_CAMERA_DEVICE_VDI_IMPL_H

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "v1_0/icamera_device_vdi.h"
#include "v1_0/icamera_device_vdi_callback.h"

namespace OHOS::Camera::Hybris {

namespace Hidl {
class HwBinderClient;
class HwBinderServer;
class HwCameraDevice;
class HwCameraDeviceCallback;
} // namespace Hidl

using OHOS::VDI::Camera::V1_0::ICameraDeviceVdi;
using OHOS::VDI::Camera::V1_0::ICameraDeviceVdiCallback;
using OHOS::VDI::Camera::V1_0::IStreamOperatorVdi;
using OHOS::VDI::Camera::V1_0::IStreamOperatorVdiCallback;
using OHOS::VDI::Camera::V1_0::VdiResultCallbackMode;

class HybrisCameraDeviceVdiImpl : public ICameraDeviceVdi {
public:
    /*
     * Constructed by HybrisCameraHostVdiImpl::OpenCamera after it has
     * (a) resolved the cameraId → Halium fq name ("device@3.6/internal/N"),
     * (b) called HwCameraProvider::GetCameraDeviceInterface, and
     * (c) acquired the returned binder handle.
     *
     * Takes a borrowed pointer to the shared HwBinderClient owned by
     * the host VDI (single /dev/hwbinder fd is reused for all camera
     * device proxies); takes ownership of the HwCameraDevice instance.
     */
    HybrisCameraDeviceVdiImpl(Hidl::HwBinderClient *client,
                              std::string ohosCameraId,
                              std::unique_ptr<Hidl::HwCameraDevice> device,
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
    /*
     * Lazy: open the Halium ICameraDeviceSession on the first
     * GetStreamOperator call.  Returns 0 on success / on already-
     * open; non-zero VdiCamRetCode on failure.
     */
    int32_t EnsureHaliumSessionOpen();

    std::mutex                              mutex_;
    /* Borrowed pointer; the stream operator (N12.6) uses this fd to
     * talk to the Halium ICameraDeviceSession after open(). */
    Hidl::HwBinderClient                   *client_;
    std::string                             ohosCameraId_;
    std::unique_ptr<Hidl::HwCameraDevice>   device_;
    sptr<ICameraDeviceVdiCallback>          deviceCallback_;
    VdiResultCallbackMode                   resultMode_ =
        VdiResultCallbackMode::PER_FRAME;
    std::vector<int32_t>                    enabledResults_;
    std::vector<uint8_t>                    latestSettings_;
    bool                                    closed_ = false;

    /*
     * Server side: HwBinderServer worker thread + the local
     * BnHwCameraDeviceCallback impl Halium calls back through.
     * Owned by the device so it dies with the device.
     */
    std::unique_ptr<Hidl::HwBinderServer>          binderServer_;
    std::unique_ptr<Hidl::HwCameraDeviceCallback>  hwCallback_;

    /* Halium ICameraDeviceSession handle (set by EnsureHaliumSessionOpen). */
    uint32_t                                       sessionHandle_ = 0;
    bool                                           sessionOpened_ = false;
};

} // namespace OHOS::Camera::Hybris

#endif // HYBRIS_CAMERA_DEVICE_VDI_IMPL_H
