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
    std::lock_guard<std::mutex> guard(lock_);
    settings_ = settings;
    // The settings only reach the HAL on the next capture request; the OHOS
    // framework re-issues its repeating capture after changing them.
    return VDI::Camera::V1_0::NO_ERROR;
}

const std::vector<uint8_t> &HybrisCameraDevice::LatestSettings()
{
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
    {
        std::lock_guard<std::mutex> guard(lock_);
        cb = callback_;
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
    auto result = BuildResultMetadata(ndk_, androidResult, enabled);
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
