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

#ifndef HYBRIS_CAMERA_DEVICE_VDI_H
#define HYBRIS_CAMERA_DEVICE_VDI_H

#include <memory>
#include <mutex>
#include <vector>

#include "v1_0/icamera_device_vdi.h"
#include "v1_0/icamera_device_vdi_callback.h"

#include "hybris_camera_metadata.h"
#include "hybris_ndk_api.h"
#include "hybris_stream_operator_vdi.h"

namespace OHOS {
namespace HybrisCamera {

using namespace OHOS::VDI::Camera::V1_0;

// One open ACameraDevice, plus the settings the framework has pushed onto it.
class HybrisCameraDevice : public ICameraDeviceVdi {
public:
    HybrisCameraDevice(const NdkApi *ndk, const CameraInfo *info,
                       const sptr<ICameraDeviceVdiCallback> &callback);
    ~HybrisCameraDevice() override;

    bool Open(ACameraManager *manager);
    bool IsOpen() const { return device_ != nullptr; }

    int32_t GetStreamOperator(const sptr<IStreamOperatorVdiCallback> &callbackObj,
                              sptr<IStreamOperatorVdi> &streamOperator) override;
    int32_t UpdateSettings(const std::vector<uint8_t> &settings) override;
    int32_t SetResultMode(VdiResultCallbackMode mode) override;
    int32_t GetEnabledResults(std::vector<int32_t> &results) override;
    int32_t EnableResult(const std::vector<int32_t> &results) override;
    int32_t DisableResult(const std::vector<int32_t> &results) override;
    int32_t Close() override;

    // Used by the stream operator.
    ACameraDevice *NdkDevice() const { return device_; }
    const CameraInfo *Info() const { return info_; }
    const std::vector<uint8_t> &LatestSettings();
    void ReportResult(const ACameraMetadata *androidResult, int64_t timestampNs);
    void ReportError(VdiErrorType type, int32_t code);

private:
    static void OnDeviceDisconnected(void *context, ACameraDevice *device);
    static void OnDeviceError(void *context, ACameraDevice *device, int error);

    const NdkApi *ndk_;
    const CameraInfo *info_;
    sptr<ICameraDeviceVdiCallback> callback_;

    std::mutex lock_;
    ACameraDevice *device_ = nullptr;
    sptr<HybrisStreamOperator> streamOperator_;
    std::vector<uint8_t> settings_;
    std::vector<int32_t> enabledResults_;
    VdiResultCallbackMode resultMode_ = PER_FRAME;
    uint64_t resultSeq_ = 0;
};

} // namespace HybrisCamera
} // namespace OHOS

#endif // HYBRIS_CAMERA_DEVICE_VDI_H
