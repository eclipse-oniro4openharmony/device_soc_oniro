/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hybris_camera_device_vdi_impl.h"

#include <algorithm>

#include "hidl/hw_binder_client.h"
#include "hidl/hw_camera_device.h"
#include "hybris_camera_log.h"
#include "hybris_stream_operator_vdi_impl.h"
#include "v1_0/vdi_types.h"

namespace OHOS::Camera::Hybris {

namespace V = OHOS::VDI::Camera::V1_0;

HybrisCameraDeviceVdiImpl::HybrisCameraDeviceVdiImpl(
    Hidl::HwBinderClient *client, std::string ohosCameraId,
    std::unique_ptr<Hidl::HwCameraDevice> device,
    const sptr<ICameraDeviceVdiCallback> &cb)
    : client_(client),
      ohosCameraId_(std::move(ohosCameraId)),
      device_(std::move(device)),
      deviceCallback_(cb)
{
    CAMERA_VDI_LOGI("HybrisCameraDeviceVdiImpl ctor for %{public}s "
                    "(halium handle=%{public}u)",
                    ohosCameraId_.c_str(),
                    device_ ? device_->Handle() : 0);
}

HybrisCameraDeviceVdiImpl::~HybrisCameraDeviceVdiImpl()
{
    CAMERA_VDI_LOGI("HybrisCameraDeviceVdiImpl dtor for %{public}s",
                    ohosCameraId_.c_str());
}

int32_t HybrisCameraDeviceVdiImpl::GetStreamOperator(
    const sptr<IStreamOperatorVdiCallback> &callbackObj,
    sptr<IStreamOperatorVdi> &streamOperator)
{
    streamOperator = nullptr;
    if (callbackObj == nullptr) {
        return V::INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || device_ == nullptr) {
        return V::CAMERA_CLOSED;
    }
    /*
     * N12.6.0: return a logging stub that accepts CreateStreams/Commit/
     * Attach so we can see what shape the OHOS framework requests
     * on real hardware.  Capture still returns METHOD_NOT_SUPPORTED
     * (preview frames not delivered until N12.7 buffer interop).
     */
    auto op = sptr<HybrisStreamOperatorVdiImpl>::MakeSptr(
        client_, device_.get(), ohosCameraId_, callbackObj);
    if (op == nullptr) {
        return V::INSUFFICIENT_RESOURCES;
    }
    streamOperator = op;
    return V::NO_ERROR;
}

int32_t HybrisCameraDeviceVdiImpl::UpdateSettings(
    const std::vector<uint8_t> &settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    latestSettings_ = settings;
    CAMERA_VDI_LOGI("UpdateSettings(%{public}s): %{public}zu bytes stashed",
                    ohosCameraId_.c_str(), latestSettings_.size());
    return V::NO_ERROR;
}

int32_t HybrisCameraDeviceVdiImpl::SetResultMode(VdiResultCallbackMode mode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    resultMode_ = mode;
    CAMERA_VDI_LOGI("SetResultMode(%{public}s): mode=%{public}d",
                    ohosCameraId_.c_str(), static_cast<int32_t>(mode));
    return V::NO_ERROR;
}

int32_t HybrisCameraDeviceVdiImpl::GetEnabledResults(
    std::vector<int32_t> &results)
{
    std::lock_guard<std::mutex> lock(mutex_);
    results = enabledResults_;
    return V::NO_ERROR;
}

int32_t HybrisCameraDeviceVdiImpl::EnableResult(
    const std::vector<int32_t> &results)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (int32_t r : results) {
        if (std::find(enabledResults_.begin(), enabledResults_.end(), r)
            == enabledResults_.end()) {
            enabledResults_.push_back(r);
        }
    }
    return V::NO_ERROR;
}

int32_t HybrisCameraDeviceVdiImpl::DisableResult(
    const std::vector<int32_t> &results)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (int32_t r : results) {
        enabledResults_.erase(
            std::remove(enabledResults_.begin(), enabledResults_.end(), r),
            enabledResults_.end());
    }
    return V::NO_ERROR;
}

int32_t HybrisCameraDeviceVdiImpl::Close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return V::NO_ERROR;
    }
    closed_ = true;
    /*
     * Releasing the unique_ptr drops our reference to HwCameraDevice;
     * its destructor doesn't release the binder ref directly (the
     * ref was acquired by HwBinderClient::ParseReplies and is owned
     * by us until we explicitly BC_RELEASE).  TODO N12.7: actually
     * BC_RELEASE the handle via client_; for now the HwBinderClient
     * teardown at VDI shutdown releases all outstanding refs.
     */
    CAMERA_VDI_LOGI("Close(%{public}s): releasing Halium device "
                    "(handle=%{public}u)",
                    ohosCameraId_.c_str(),
                    device_ ? device_->Handle() : 0);
    device_.reset();
    deviceCallback_ = nullptr;
    return V::NO_ERROR;
}

} // namespace OHOS::Camera::Hybris
