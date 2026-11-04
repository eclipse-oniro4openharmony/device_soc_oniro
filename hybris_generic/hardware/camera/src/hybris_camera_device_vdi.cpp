/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hybris_camera_device_vdi.h"

#include <algorithm>

#include "metadata_utils.h"

#include "hybris_camera_common.h"

namespace OHOS {
namespace HybrisCamera {

HybrisCameraDevice::HybrisCameraDevice(const NdkApi *ndk, const CameraInfo *info,
                                       const sptr<ICameraDeviceVdiCallback> &callback)
    : ndk_(ndk), info_(info), callback_(callback)
{
    enabledResults_ = SupportedResultTags();
}

HybrisCameraDevice::~HybrisCameraDevice()
{
    (void)Close();
}

bool HybrisCameraDevice::Open(ACameraManager *manager)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (device_ != nullptr) {
        return true;
    }
    if (manager == nullptr) {
        return false;
    }
    manager_ = manager;
    lens_ = 0;
    ACameraDevice_StateCallbacks stateCb {};
    stateCb.context = this;
    stateCb.onDisconnected = OnDeviceDisconnected;
    stateCb.onError = OnDeviceError;

    camera_status_t st =
        ndk_->ACameraManager_openCamera(manager, info_->androidId.c_str(), &stateCb, &device_);
    if (st != ACAMERA_OK) {
        HC_LOGE("openCamera(%{public}s) -> %{public}s", info_->androidId.c_str(), NdkStatusName(st));
        device_ = nullptr;
        return false;
    }
    return true;
}

bool HybrisCameraDevice::SwitchLens(int lens)
{
    if (lens < 0 || static_cast<size_t>(lens) >= info_->lenses.size()) {
        return false;
    }
    ACameraDevice *old = nullptr;
    int previous = 0;
    ACameraManager *manager = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (lens == lens_ && device_ != nullptr) {
            return true;
        }
        old = device_;
        device_ = nullptr;
        previous = lens_;
        manager = manager_;
    }
    // Closing disconnects the readers' windows from the old camera, so the
    // next session can take them over.
    if (old != nullptr) {
        ndk_->ACameraDevice_close(old);
    }
    if (manager == nullptr) {
        return false;
    }
    ACameraDevice_StateCallbacks stateCb {};
    stateCb.context = this;
    stateCb.onDisconnected = OnDeviceDisconnected;
    stateCb.onError = OnDeviceError;
    ACameraDevice *dev = nullptr;
    const std::string &id = info_->lenses[lens].androidId;
    camera_status_t st = ndk_->ACameraManager_openCamera(manager, id.c_str(), &stateCb, &dev);
    bool ok = st == ACAMERA_OK && dev != nullptr;
    if (!ok) {
        HC_LOGE("%{public}s: opening the %{public}s lens (android %{public}s) failed: %{public}s",
                info_->vdiId.c_str(), info_->lenses[lens].role.c_str(), id.c_str(), NdkStatusName(st));
        lens = previous;
        dev = nullptr;
        st = ndk_->ACameraManager_openCamera(manager, info_->lenses[lens].androidId.c_str(), &stateCb, &dev);
        if (st != ACAMERA_OK) {
            dev = nullptr;
        }
    }
    std::lock_guard<std::mutex> guard(lock_);
    device_ = dev;
    lens_ = lens;
    if (ok) {
        HC_LOGI("%{public}s: now on the %{public}s lens (android %{public}s)", info_->vdiId.c_str(),
                info_->lenses[lens].role.c_str(), id.c_str());
    }
    return ok;
}

int32_t HybrisCameraDevice::GetStreamOperator(const sptr<IStreamOperatorVdiCallback> &callbackObj,
                                              sptr<IStreamOperatorVdi> &streamOperator)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (device_ == nullptr) {
        return VDI::Camera::V1_0::CAMERA_CLOSED;
    }
    if (streamOperator_ == nullptr) {
        streamOperator_ = new (std::nothrow) HybrisStreamOperator(ndk_, this, callbackObj);
        if (streamOperator_ == nullptr) {
            return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
        }
    }
    streamOperator = streamOperator_;
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraDevice::UpdateSettings(const std::vector<uint8_t> &settings)
{
    sptr<HybrisStreamOperator> op;
    {
        std::lock_guard<std::mutex> guard(lock_);
        // camera_service sends only what changed since its previous call.
        MergeSettings(settings_, settings);
        op = streamOperator_;
    }
    // A running preview has to pick the change up now (zoom, flash, macro
    // ...): camera_service does not restart its repeating capture for it.
    if (op != nullptr) {
        op->SettingsChanged();
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

std::vector<uint8_t> HybrisCameraDevice::LatestSettings()
{
    std::lock_guard<std::mutex> guard(lock_);
    return settings_;
}

int32_t HybrisCameraDevice::SetResultMode(VdiResultCallbackMode mode)
{
    std::lock_guard<std::mutex> guard(lock_);
    resultMode_ = mode;
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraDevice::GetEnabledResults(std::vector<int32_t> &results)
{
    std::lock_guard<std::mutex> guard(lock_);
    results = enabledResults_;
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraDevice::EnableResult(const std::vector<int32_t> &results)
{
    std::lock_guard<std::mutex> guard(lock_);
    for (int32_t tag : results) {
        if (std::find(enabledResults_.begin(), enabledResults_.end(), tag) == enabledResults_.end()) {
            enabledResults_.push_back(tag);
        }
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraDevice::DisableResult(const std::vector<int32_t> &results)
{
    std::lock_guard<std::mutex> guard(lock_);
    for (int32_t tag : results) {
        auto it = std::find(enabledResults_.begin(), enabledResults_.end(), tag);
        if (it != enabledResults_.end()) {
            enabledResults_.erase(it);
        }
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraDevice::Close()
{
    sptr<HybrisStreamOperator> op;
    ACameraDevice *dev = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        op = streamOperator_;
        streamOperator_ = nullptr;
        dev = device_;
        device_ = nullptr;
    }
    // Sessions must go before the device or camera2 complains on close.
    if (op != nullptr) {
        op->Shutdown();
    }
    if (dev != nullptr) {
        ndk_->ACameraDevice_close(dev);
        HC_LOGI("closed camera %{public}s", info_->vdiId.c_str());
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

void HybrisCameraDevice::ReportResult(const ACameraMetadata *androidResult, int64_t timestampNs)
{
    sptr<ICameraDeviceVdiCallback> cb;
    std::vector<int32_t> enabled;
    float zoomScale = 1.0f;
    {
        std::lock_guard<std::mutex> guard(lock_);
        cb = callback_;
        zoomScale = info_->lenses[lens_].scale;
        enabled = enabledResults_;
        resultSeq_++;
        // ON_CHANGED is a hint that the framework only wants updates; sending
        // every frame is still correct, just chattier, so honour it cheaply by
        // decimating rather than diffing metadata on every frame.
        constexpr uint64_t ON_CHANGED_DECIMATION = 10;
        if (resultMode_ == ON_CHANGED && (resultSeq_ % ON_CHANGED_DECIMATION) != 1) {
            return;
        }
    }
    if (cb == nullptr) {
        return;
    }
    auto result = BuildResultMetadata(ndk_, androidResult, enabled, zoomScale);
    if (result == nullptr) {
        return;
    }
    std::vector<uint8_t> encoded;
    if (!OHOS::Camera::MetadataUtils::ConvertMetadataToVec(result, encoded)) {
        return;
    }
    cb->OnResult(static_cast<uint64_t>(timestampNs), encoded);
}

void HybrisCameraDevice::ReportError(VdiErrorType type, int32_t code)
{
    sptr<ICameraDeviceVdiCallback> cb;
    {
        std::lock_guard<std::mutex> guard(lock_);
        cb = callback_;
    }
    if (cb != nullptr) {
        cb->OnError(type, code);
    }
}

void HybrisCameraDevice::OnDeviceDisconnected(void *context, ACameraDevice * /*device*/)
{
    auto *self = static_cast<HybrisCameraDevice *>(context);
    if (self == nullptr) {
        return;
    }
    HC_LOGE("camera %{public}s disconnected", self->info_->vdiId.c_str());
    self->ReportError(VdiErrorType::DEVICE_DISCONNECT, 0);
}

void HybrisCameraDevice::OnDeviceError(void *context, ACameraDevice * /*device*/, int error)
{
    auto *self = static_cast<HybrisCameraDevice *>(context);
    if (self == nullptr) {
        return;
    }
    HC_LOGE("camera %{public}s error %{public}d", self->info_->vdiId.c_str(), error);
    self->ReportError(VdiErrorType::FATAL_ERROR, error);
}

} // namespace HybrisCamera
} // namespace OHOS
