/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hybris_camera_device_vdi_impl.h"

#include <algorithm>

#include "hidl/hw_binder_client.h"
#include "hidl/hw_binder_server.h"
#include "hidl/hw_camera_device.h"
#include "hidl/hw_camera_device_callback.h"
#include "hidl/hw_camera_device_session.h"
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

    /*
     * Spin up the local binder server.  Halium calls back into us
     * through this for processCaptureResult / notify after
     * ICameraDevice::open() — so the server MUST be live BEFORE
     * EnsureHaliumSessionOpen issues the open() transaction.
     *
     * Also wire the client to forward inline BR_TRANSACTIONs that
     * arrive during a client-thread BWR (nested-call pattern: the
     * kernel routes Halium's callback to the same thread that's
     * waiting on the open() reply, to avoid deadlock).
     */
    if (client_ != nullptr) {
        binderServer_ = std::make_unique<Hidl::HwBinderServer>(client_->Fd());
        hwCallback_   = std::make_unique<Hidl::HwCameraDeviceCallback>();
        binderServer_->Register(hwCallback_.get());
        if (!binderServer_->Start()) {
            CAMERA_VDI_LOGE("Failed to start HwBinderServer for %{public}s",
                            ohosCameraId_.c_str());
            binderServer_.reset();
            hwCallback_.reset();
        } else {
            client_->SetServer(binderServer_.get());
        }
    }
}

HybrisCameraDeviceVdiImpl::~HybrisCameraDeviceVdiImpl()
{
    CAMERA_VDI_LOGI("HybrisCameraDeviceVdiImpl dtor for %{public}s",
                    ohosCameraId_.c_str());
    if (client_ != nullptr) {
        client_->SetServer(nullptr);
    }
    binderServer_.reset();
    hwCallback_.reset();
}

int32_t HybrisCameraDeviceVdiImpl::EnsureHaliumSessionOpen()
{
    /* Caller holds mutex_. */
    if (session_ != nullptr) {
        return V::NO_ERROR;
    }
    if (device_ == nullptr) {
        return V::CAMERA_CLOSED;
    }
    if (hwCallback_ == nullptr || binderServer_ == nullptr) {
        CAMERA_VDI_LOGE("EnsureHaliumSessionOpen(%{public}s): no callback "
                        "infrastructure", ohosCameraId_.c_str());
        return V::DEVICE_ERROR;
    }
    int32_t halStatus = -1;
    uint32_t sessionHandle = 0;
    bool isNull = true;
    if (!device_->Open(hwCallback_->Key(), &halStatus,
                       &sessionHandle, &isNull)) {
        CAMERA_VDI_LOGE("EnsureHaliumSessionOpen(%{public}s): Open failed "
                        "(halStatus=%{public}d)",
                        ohosCameraId_.c_str(), halStatus);
        return V::DEVICE_ERROR;
    }
    if (isNull || sessionHandle == 0) {
        CAMERA_VDI_LOGE("EnsureHaliumSessionOpen(%{public}s): null session",
                        ohosCameraId_.c_str());
        return V::DEVICE_ERROR;
    }
    session_ = std::make_unique<Hidl::HwCameraDeviceSession>(client_,
                                                              sessionHandle);
    CAMERA_VDI_LOGI("EnsureHaliumSessionOpen(%{public}s): "
                    "ICameraDeviceSession handle=%{public}u",
                    ohosCameraId_.c_str(), sessionHandle);

    /*
     * Liveness check: pull a default PREVIEW request template.  This
     * is a small constructDefaultRequestSettings call (V3.2 TX 1)
     * that returns ~hundreds of bytes of camera_metadata_t.  If it
     * succeeds, the session is responsive and we can attempt
     * configureStreams_3_4 in N12.6.1b.
     */
    int32_t tmplStatus = -1;
    std::vector<uint8_t> tmplBlob;
    if (session_->ConstructDefaultRequestSettings(
            Hidl::HwCameraDeviceSession::TEMPLATE_PREVIEW,
            &tmplStatus, &tmplBlob)) {
        CAMERA_VDI_LOGI("PREVIEW template: %{public}zu bytes (session live)",
                        tmplBlob.size());
    } else {
        CAMERA_VDI_LOGW("PREVIEW template fetch failed (halStatus=%{public}d)",
                        tmplStatus);
    }

    return V::NO_ERROR;
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
     * N12.6.1: lazily open the Halium ICameraDeviceSession on first
     * GetStreamOperator call.  This is the gating step for actual
     * frame delivery — without a session handle there's no
     * configureStreams to bridge.
     */
    int32_t openRc = EnsureHaliumSessionOpen();
    if (openRc != V::NO_ERROR) {
        return openRc;
    }

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
