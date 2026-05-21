/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hybris_camera_device_vdi_impl.h"

#include <algorithm>

#include "droidmedia/droidmedia_loader.h"
#include "hybris_camera_log.h"
#include "hybris_stream_operator_vdi_impl.h"
#include "v1_0/vdi_types.h"

namespace OHOS::Camera::Hybris {

namespace V = OHOS::VDI::Camera::V1_0;

HybrisCameraDeviceVdiImpl::HybrisCameraDeviceVdiImpl(
    std::string ohosCameraId, DroidMediaCamera *cam,
    const sptr<ICameraDeviceVdiCallback> &cb)
    : ohosCameraId_(std::move(ohosCameraId)), cam_(cam), deviceCallback_(cb)
{
    CAMERA_VDI_LOGI("HybrisCameraDeviceVdiImpl ctor for %{public}s cam=%{public}p",
                    ohosCameraId_.c_str(), (void *)cam_);
}

HybrisCameraDeviceVdiImpl::~HybrisCameraDeviceVdiImpl()
{
    CAMERA_VDI_LOGI("HybrisCameraDeviceVdiImpl dtor for %{public}s",
                    ohosCameraId_.c_str());
    /*
     * Defensive: if Close() wasn't called (framework crashed or
     * skipped teardown), still release the droidmedia handle so the
     * Halium HAL doesn't keep the sensor powered on.
     */
    if (cam_ != nullptr) {
        Droid::Loader::Get().Disconnect(cam_);
        cam_ = nullptr;
    }
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
    if (closed_ || cam_ == nullptr) {
        return V::CAMERA_CLOSED;
    }
    auto op = sptr<HybrisStreamOperatorVdiImpl>::MakeSptr(
        cam_, ohosCameraId_, callbackObj);
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
    /*
     * TODO N12.D follow-up: parse OHOS camera_metadata_t entries we
     * actually care about (auto-focus mode, exposure compensation,
     * scene mode) and translate to Camera1 setParameters calls.
     * MVP just stashes — the HAL runs in auto mode by default which
     * is acceptable for first frames.
     */
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
    if (cam_ != nullptr) {
        CAMERA_VDI_LOGI("Close(%{public}s): droid_media_camera_disconnect(%{public}p)",
                        ohosCameraId_.c_str(), (void *)cam_);
        Droid::Loader::Get().Disconnect(cam_);
        cam_ = nullptr;
    }
    deviceCallback_ = nullptr;
    return V::NO_ERROR;
}

} // namespace OHOS::Camera::Hybris
