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

/*
 * ICameraHostVdi over the Halium camera stack.
 *
 * camera_host loads this through HdfLoadVdi (the name comes from vdiLibList in
 * /vendor/etc/hdfconfig/camera_host_config.hcb) and everything above — the
 * stock HDI service, SA 3008, the camera framework — is unmodified.  Below us
 * is the container's CameraService, reached with the camera2 NDK hosted by
 * libhybris.  See device/board/oniro/docs/hybris_generic/camera_enablement_plan.md.
 */

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "hdf_load_vdi.h"
#include "metadata_utils.h"
#include "parameter.h"

#include "v1_0/icamera_host_vdi.h"

#include "hybris_camera_common.h"
#include "hybris_camera_device_vdi.h"
#include "hybris_camera_metadata.h"
#include "hybris_camera_service_proxy.h"
#include "hybris_ndk_api.h"

namespace OHOS {
namespace HybrisCamera {

using namespace OHOS::VDI::Camera::V1_0;

class HybrisCameraHost : public ICameraHostVdi {
public:
    HybrisCameraHost() = default;
    ~HybrisCameraHost() override;

    // Kicks off the background bring-up; never blocks camera_host's start.
    void Init();

    int32_t SetCallback(const sptr<ICameraHostVdiCallback> &callbackObj) override;
    int32_t GetCameraIds(std::vector<std::string> &cameraIds) override;
    int32_t GetCameraAbility(const std::string &cameraId, std::vector<uint8_t> &cameraAbility) override;
    int32_t OpenCamera(const std::string &cameraId, const sptr<ICameraDeviceVdiCallback> &callbackObj,
                       sptr<ICameraDeviceVdi> &device) override;
    int32_t SetFlashlight(const std::string &cameraId, bool isEnable) override;
    int32_t CloseAllCameras() override;

private:
    void BringUp();
    bool WaitForAndroid();
    bool Enumerate();
    CameraInfo *FindCamera(const std::string &cameraId);

    std::mutex lock_;
    const NdkApi *ndk_ = nullptr;
    ACameraManager *manager_ = nullptr;
    std::vector<std::shared_ptr<CameraInfo>> cameras_;
    std::map<std::string, sptr<HybrisCameraDevice>> openDevices_;
    sptr<ICameraHostVdiCallback> callback_;
    std::thread bringUpThread_;
    bool enabled_ = false;
    bool ready_ = false;
};

namespace {

constexpr int PARAM_VALUE_MAX = 64;

bool ParamEquals(const char *key, const char *expected)
{
    char value[PARAM_VALUE_MAX] = { 0 };
    int len = GetParameter(key, "", value, sizeof(value));
    return len > 0 && strcmp(value, expected) == 0;
}

} // namespace

HybrisCameraHost::~HybrisCameraHost()
{
    if (bringUpThread_.joinable()) {
        bringUpThread_.join();
    }
    (void)CloseAllCameras();
}

void HybrisCameraHost::Init()
{
    enabled_ = ParamEquals(HDI_IMPL_PARAM, HDI_IMPL_VDI);
    if (!enabled_) {
        // Dormant: camera_host binds cleanly and reports no cameras, which is
        // exactly the pre-existing behaviour on devices without this bridge.
        HC_LOGI("%{public}s is not \"%{public}s\" — hybris camera bridge stays dormant",
                HDI_IMPL_PARAM, HDI_IMPL_VDI);
        return;
    }
    HC_LOGI("hybris camera bridge enabled, bringing up in the background");
    bringUpThread_ = std::thread([this]() { BringUp(); });
}

bool HybrisCameraHost::WaitForAndroid()
{
    // Same signal the display and audio bridges wait on: androidd raises it
    // once the container's HAL fleet is up.  minimediaservice (CameraService)
    // and camerahalserver come up in the same wave.
    for (int i = 0; i < ANDROID_READY_TIMEOUT_S; ++i) {
        if (ParamEquals(ANDROID_READY_PARAM, "1")) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    HC_LOGE("%{public}s never came up; giving up on the camera bridge", ANDROID_READY_PARAM);
    return false;
}

void HybrisCameraHost::BringUp()
{
    if (!WaitForAndroid()) {
        return;
    }
    const NdkApi *ndk = LoadNdkApi();
    if (ndk == nullptr) {
        return;
    }

    // Without a media.camera.proxy in the container, CameraService's
    // device-policy query fails closed and every connectDevice() is refused
    // with ERROR_DISABLED.  Halium has no system_server to provide it.
    SetProxyLogger([](const char *msg) { HC_LOGI("%{public}s", msg); });
    if (!EnsureCameraServiceProxy()) {
        HC_LOGE("could not register media.camera.proxy — openCamera will be refused");
    }

    ACameraManager *manager = ndk->ACameraManager_create();
    if (manager == nullptr) {
        HC_LOGE("ACameraManager_create returned null");
        return;
    }
    {
        std::lock_guard<std::mutex> guard(lock_);
        ndk_ = ndk;
        manager_ = manager;
    }

    // CameraService may register after us; poll rather than fail the bind.
    constexpr int ENUM_ATTEMPTS = 30;
    for (int attempt = 0; attempt < ENUM_ATTEMPTS; ++attempt) {
        if (Enumerate()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    HC_LOGE("no cameras appeared after %{public}d attempts", ENUM_ATTEMPTS);
}

bool HybrisCameraHost::Enumerate()
{
    ACameraIdList *ids = nullptr;
    camera_status_t st = ndk_->ACameraManager_getCameraIdList(manager_, &ids);
    if (st != ACAMERA_OK || ids == nullptr) {
        HC_LOGW("getCameraIdList -> %{public}s", NdkStatusName(st));
        return false;
    }
    if (ids->numCameras <= 0) {
        ndk_->ACameraManager_deleteCameraIdList(ids);
        return false;
    }

    // Only the primary back and front cameras are exposed; the two back
    // auxiliaries the MTK HAL advertises have no place in the OHOS model yet
    // (plan §D6).
    std::vector<std::shared_ptr<CameraInfo>> found;
    bool haveBack = false;
    bool haveFront = false;
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata *chars = nullptr;
        if (ndk_->ACameraManager_getCameraCharacteristics(manager_, ids->cameraIds[i], &chars) !=
                ACAMERA_OK || chars == nullptr) {
            continue;
        }
        auto info = std::make_shared<CameraInfo>();
        info->androidId = ids->cameraIds[i];
        info->vdiId = std::string("android:") + info->androidId;
        bool ok = BuildCameraAbility(ndk_, chars, *info);
        ndk_->ACameraMetadata_free(chars);
        if (!ok) {
            continue;
        }
        if (info->position == OHOS_CAMERA_POSITION_BACK && !haveBack) {
            info->vdiId = CAMERA_ID_BACK;
            haveBack = true;
        } else if (info->position == OHOS_CAMERA_POSITION_FRONT && !haveFront) {
            info->vdiId = CAMERA_ID_FRONT;
            haveFront = true;
        } else {
            // Auxiliary back cameras: the OHOS model has no place for them yet.
            continue;
        }
        found.push_back(info);
    }
    ndk_->ACameraManager_deleteCameraIdList(ids);

    if (found.empty()) {
        return false;
    }
    // Back first so it becomes lcam001 — the framework's default camera.
    std::sort(found.begin(), found.end(),
              [](const std::shared_ptr<CameraInfo> &a, const std::shared_ptr<CameraInfo> &b) {
                  return a->position == OHOS_CAMERA_POSITION_BACK &&
                         b->position != OHOS_CAMERA_POSITION_BACK;
              });

    sptr<ICameraHostVdiCallback> cb;
    {
        std::lock_guard<std::mutex> guard(lock_);
        cameras_ = found;
        ready_ = true;
        cb = callback_;
    }
    HC_LOGI("hybris camera bridge ready with %{public}zu camera(s)", found.size());
    if (cb != nullptr) {
        for (const auto &info : found) {
            cb->OnCameraEvent(info->vdiId, CAMERA_EVENT_DEVICE_ADD);
            cb->OnCameraStatus(info->vdiId, VdiCameraStatus::AVAILABLE);
        }
    }
    return true;
}

CameraInfo *HybrisCameraHost::FindCamera(const std::string &cameraId)
{
    for (const auto &info : cameras_) {
        if (info->vdiId == cameraId) {
            return info.get();
        }
    }
    return nullptr;
}

int32_t HybrisCameraHost::SetCallback(const sptr<ICameraHostVdiCallback> &callbackObj)
{
    std::vector<std::shared_ptr<CameraInfo>> known;
    {
        std::lock_guard<std::mutex> guard(lock_);
        callback_ = callbackObj;
        known = cameras_;
    }
    // Enumeration may already have finished while nobody was listening; the
    // framework only learns about cameras through this callback.
    if (callbackObj != nullptr) {
        for (const auto &info : known) {
            callbackObj->OnCameraStatus(info->vdiId, VdiCameraStatus::AVAILABLE);
        }
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHost::GetCameraIds(std::vector<std::string> &cameraIds)
{
    std::lock_guard<std::mutex> guard(lock_);
    cameraIds.clear();
    for (const auto &info : cameras_) {
        cameraIds.push_back(info->vdiId);
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHost::GetCameraAbility(const std::string &cameraId,
                                           std::vector<uint8_t> &cameraAbility)
{
    std::lock_guard<std::mutex> guard(lock_);
    CameraInfo *info = FindCamera(cameraId);
    if (info == nullptr || info->ability == nullptr) {
        HC_LOGE("no ability for %{public}s", cameraId.c_str());
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }
    if (!OHOS::Camera::MetadataUtils::ConvertMetadataToVec(info->ability, cameraAbility)) {
        return VDI::Camera::V1_0::DEVICE_ERROR;
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHost::OpenCamera(const std::string &cameraId,
                                     const sptr<ICameraDeviceVdiCallback> &callbackObj,
                                     sptr<ICameraDeviceVdi> &device)
{
    sptr<HybrisCameraDevice> hybrisDevice;
    ACameraManager *manager = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        CameraInfo *info = FindCamera(cameraId);
        if (info == nullptr || ndk_ == nullptr) {
            HC_LOGE("open of unknown camera %{public}s", cameraId.c_str());
            return VDI::Camera::V1_0::INVALID_ARGUMENT;
        }
        auto existing = openDevices_.find(cameraId);
        if (existing != openDevices_.end() && existing->second->IsOpen()) {
            HC_LOGW("camera %{public}s is already open", cameraId.c_str());
            return VDI::Camera::V1_0::CAMERA_BUSY;
        }
        hybrisDevice = new (std::nothrow) HybrisCameraDevice(ndk_, info, callbackObj);
        if (hybrisDevice == nullptr) {
            return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
        }
        manager = manager_;
    }

    if (!hybrisDevice->Open(manager)) {
        return VDI::Camera::V1_0::DEVICE_ERROR;
    }
    {
        std::lock_guard<std::mutex> guard(lock_);
        openDevices_[cameraId] = hybrisDevice;
    }
    device = hybrisDevice;
    HC_LOGI("opened camera %{public}s", cameraId.c_str());
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHost::SetFlashlight(const std::string &cameraId, bool isEnable)
{
    // camera2 has no torch entry point; the decision on how to drive it (a
    // short-lived torch session vs. an ICameraService::setTorchMode shim) is
    // deferred, so report it honestly instead of silently doing nothing.
    HC_LOGW("SetFlashlight(%{public}s, %{public}d) is not implemented yet", cameraId.c_str(),
            isEnable ? 1 : 0);
    return VDI::Camera::V1_0::METHOD_NOT_SUPPORTED;
}

int32_t HybrisCameraHost::CloseAllCameras()
{
    std::map<std::string, sptr<HybrisCameraDevice>> devices;
    {
        std::lock_guard<std::mutex> guard(lock_);
        devices.swap(openDevices_);
    }
    for (auto &entry : devices) {
        (void)entry.second->Close();
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

// ─── HDF VDI plumbing ───────────────────────────────────────────────────────

static int CreateCameraHostVdiInstance(struct HdfVdiBase *vdiBase)
{
    auto *wrapper = reinterpret_cast<struct VdiWrapperCameraHost *>(vdiBase);
    auto *host = new (std::nothrow) HybrisCameraHost();
    if (host == nullptr) {
        HC_LOGE("failed to allocate the hybris camera host");
        return HDF_FAILURE;
    }
    host->Init();
    wrapper->module = host;
    return HDF_SUCCESS;
}

static int DestoryCameraHostVdiInstance(struct HdfVdiBase *vdiBase)
{
    auto *wrapper = reinterpret_cast<struct VdiWrapperCameraHost *>(vdiBase);
    auto *host = reinterpret_cast<HybrisCameraHost *>(wrapper->module);
    delete host;
    wrapper->module = nullptr;
    return HDF_SUCCESS;
}

static struct VdiWrapperCameraHost g_vdiCameraHost = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "HybrisCameraHostVdi",
        .CreateVdiInstance = CreateCameraHostVdiInstance,
        .DestoryVdiInstance = DestoryCameraHostVdiInstance,
    },
    .module = nullptr,
};

} // namespace HybrisCamera
} // namespace OHOS

HDF_VDI_INIT(OHOS::HybrisCamera::g_vdiCameraHost);
