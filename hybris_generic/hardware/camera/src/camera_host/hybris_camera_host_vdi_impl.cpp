/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hybris_camera_host_vdi_impl.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "camera_device/hybris_camera_device_vdi_impl.h"
#include "droidmedia/droidmedia_loader.h"
#include "hybris_camera_ability.h"
#include "hybris_camera_log.h"
#include "v1_0/vdi_types.h"

namespace OHOS::Camera::Hybris {

using namespace OHOS::VDI::Camera::V1_0;

HybrisCameraHostVdiImpl::HybrisCameraHostVdiImpl()
{
    CAMERA_VDI_LOGI("HybrisCameraHostVdiImpl ctor");
}

HybrisCameraHostVdiImpl::~HybrisCameraHostVdiImpl()
{
    CAMERA_VDI_LOGI("HybrisCameraHostVdiImpl dtor");
}

bool HybrisCameraHostVdiImpl::LoadCamerasFromDroidMedia()
{
    auto &loader = Droid::Loader::Get();
    if (!loader.Ready()) {
        CAMERA_VDI_LOGE("LoadCamerasFromDroidMedia: Droid::Loader not ready");
        return false;
    }
    int n = loader.GetNumberOfCameras();
    if (n <= 0) {
        CAMERA_VDI_LOGE("LoadCamerasFromDroidMedia: droid_media_camera_get_number_of_cameras "
                        "returned %{public}d", n);
        return false;
    }

    cameraIds_.clear();
    perCameraInfo_.clear();
    cameraIds_.reserve(n);
    perCameraInfo_.reserve(n);

    std::ostringstream summary;
    for (int i = 0; i < n; ++i) {
        DroidMediaCameraInfo info{};
        if (!loader.GetInfo(&info, i)) {
            CAMERA_VDI_LOGW("LoadCamerasFromDroidMedia: get_info(%{public}d) failed — skipping",
                            i);
            continue;
        }
        std::string id = std::to_string(i);
        cameraIds_.push_back(id);
        perCameraInfo_.push_back(PerCameraInfo{
            .droidIndex  = i,
            .facing      = info.facing,
            .orientation = info.orientation,
        });
        if (i) summary << ",";
        summary << id << "(facing=" << info.facing
                << ",orient=" << info.orientation << ")";
    }
    if (cameraIds_.empty()) {
        CAMERA_VDI_LOGE("LoadCamerasFromDroidMedia: every get_info call failed");
        return false;
    }
    CAMERA_VDI_LOGI("LoadCamerasFromDroidMedia: %{public}zu cameras: %{public}s",
                    cameraIds_.size(), summary.str().c_str());
    return true;
}

int32_t HybrisCameraHostVdiImpl::Init()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!LoadCamerasFromDroidMedia()) {
        cameraIds_       = { "lcam001" };
        perCameraInfo_   = { PerCameraInfo{} };
        fallback_        = true;
        CAMERA_VDI_LOGW("VDI registered with fallback cameraIds=%{public}s",
                        cameraIds_[0].c_str());
        return VDI::Camera::V1_0::NO_ERROR;
    }
    CAMERA_VDI_LOGI("VDI registered, %{public}zu droidmedia cameras",
                    cameraIds_.size());
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHostVdiImpl::SetCallback(const sptr<ICameraHostVdiCallback> &callbackObj)
{
    if (callbackObj == nullptr) {
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }
    int32_t rc = ICameraHostVdi::SetCallback(callbackObj);
    if (rc != VDI::Camera::V1_0::NO_ERROR) {
        CAMERA_VDI_LOGW("SetCallback death recipient registration failed rc=%{public}d", rc);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    hostCallback_ = callbackObj;
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHostVdiImpl::GetCameraIds(std::vector<std::string> &cameraIds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cameraIds = cameraIds_;
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHostVdiImpl::GetCameraAbility(const std::string &cameraId,
                                                  std::vector<uint8_t> &cameraAbility)
{
    /*
     * Hand-rolled ability blob per the X23 profile table — see
     * hybris_camera_ability.cpp.  Future work: parse the
     * droid_media_camera_get_parameters string to derive supported
     * preview sizes / picture sizes / FPS ranges instead of
     * hard-coding.  Orientation/facing already come from
     * perCameraInfo_ populated at Init time.
     */
    if (!BuildCameraAbility(cameraId, cameraAbility)) {
        CAMERA_VDI_LOGE("GetCameraAbility(%{public}s): no profile",
                        cameraId.c_str());
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

const HybrisCameraHostVdiImpl::PerCameraInfo *
HybrisCameraHostVdiImpl::FindInfo(const std::string &cameraId) const
{
    for (size_t i = 0; i < cameraIds_.size(); ++i) {
        if (cameraIds_[i] == cameraId) {
            return &perCameraInfo_[i];
        }
    }
    return nullptr;
}

int32_t HybrisCameraHostVdiImpl::OpenCamera(const std::string &cameraId,
                                            const sptr<ICameraDeviceVdiCallback> &callbackObj,
                                            sptr<ICameraDeviceVdi> &device)
{
    device = nullptr;
    if (callbackObj == nullptr) {
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }

    std::unique_lock<std::mutex> lock(mutex_);

    if (fallback_) {
        CAMERA_VDI_LOGE("OpenCamera(%{public}s): fallback mode "
                        "(Droid::Loader bring-up failed at Init)",
                        cameraId.c_str());
        return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
    }

    const PerCameraInfo *info = FindInfo(cameraId);
    if (info == nullptr || info->droidIndex < 0) {
        CAMERA_VDI_LOGE("OpenCamera(%{public}s): unknown cameraId",
                        cameraId.c_str());
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }

    auto &loader = Droid::Loader::Get();
    if (!loader.Ready()) {
        CAMERA_VDI_LOGE("OpenCamera(%{public}s): Droid::Loader not ready",
                        cameraId.c_str());
        return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
    }

    DroidMediaCamera *cam = loader.Connect(info->droidIndex);
    if (cam == nullptr) {
        CAMERA_VDI_LOGE("OpenCamera(%{public}s): droid_media_camera_connect(%{public}d) "
                        "returned NULL", cameraId.c_str(), info->droidIndex);
        return VDI::Camera::V1_0::DEVICE_ERROR;
    }

    /*
     * Diagnostic: dump the Camera1 parameter string the HAL is willing
     * to serve.  Knowing the supported preview-sizes / picture-sizes
     * informs the CommitStreams set_parameters call later (and tells
     * us when the HAL is too restrictive for the OHOS framework's
     * 1280x720 preview request).
     */
    if (char *params = loader.GetParameters(cam)) {
        /*
         * Dump in 512-byte chunks so the full Camera1 parameter string
         * (typically 2-4 KB on MTK HALs) survives hilog's per-line cap.
         * Knowing the supported preview-size-values is essential for
         * CommitStreams to pick a setParameters value the HAL accepts.
         * Some HALs leak the returned pointer; some expect free() —
         * upstream droidmedia returns strdup-allocated memory, so we
         * free.
         */
        size_t len = std::strlen(params);
        constexpr size_t kChunk = 512;
        for (size_t off = 0; off < len; off += kChunk) {
            size_t n = std::min(kChunk, len - off);
            std::string chunk(params + off, n);
            CAMERA_VDI_LOGI("OpenCamera(%{public}s): params[%{public}zu..%{public}zu] %{public}s",
                            cameraId.c_str(), off, off + n, chunk.c_str());
        }
        std::free(params);
    }

    /* Drop the host lock before constructing the device wrapper (it
     * may take its own internal lock; we don't want two held at once). */
    lock.unlock();

    auto impl = sptr<HybrisCameraDeviceVdiImpl>::MakeSptr(cameraId, cam, callbackObj);
    if (impl == nullptr) {
        CAMERA_VDI_LOGE("OpenCamera(%{public}s): VDI device alloc failed",
                        cameraId.c_str());
        loader.Disconnect(cam);
        return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
    }
    device = impl;
    CAMERA_VDI_LOGI("OpenCamera(%{public}s → droid idx=%{public}d): "
                    "device wrapper constructed (cam=%{public}p)",
                    cameraId.c_str(), info->droidIndex, (void *)cam);
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHostVdiImpl::SetFlashlight(const std::string &cameraId, bool isEnable)
{
    (void)cameraId;
    (void)isEnable;
    /*
     * The on-device libdroidmedia.so revision (Halium X23 2026-02-04)
     * does NOT export droid_media_camera_set_torch_mode.  Future work
     * (N12.D.9): either (a) move to a newer droidmedia build, or
     * (b) drive the LED via the sysfs class node directly from a
     * separate flashlight VDI.  For now we stay METHOD_NOT_SUPPORTED.
     */
    CAMERA_VDI_LOGW("SetFlashlight: droid_media_camera_set_torch_mode "
                    "not exported by this libdroidmedia build");
    return VDI::Camera::V1_0::METHOD_NOT_SUPPORTED;
}

int32_t HybrisCameraHostVdiImpl::CloseAllCameras()
{
    /*
     * Device VDIs hold their own DroidMediaCamera* and disconnect in
     * Close().  Host-level CloseAllCameras is a hint from the death
     * recipient ICameraHostVdi base class; framework retains the per-
     * device sptr through camera_host_service so the underlying
     * Close() will fire naturally.
     */
    CAMERA_VDI_LOGI("CloseAllCameras — no-op (per-device Close handles teardown)");
    return VDI::Camera::V1_0::NO_ERROR;
}

} // namespace OHOS::Camera::Hybris

/* -------------------------------------------------------------------------
 * HDF VDI plumbing — exported so libcamera_host_service can dlopen + bind.
 * ------------------------------------------------------------------------- */
extern "C" {

static int CreateCameraHostVdiInstance(struct HdfVdiBase *vdiBase)
{
    using OHOS::VDI::Camera::V1_0::VdiWrapperCameraHost;
    using OHOS::Camera::Hybris::HybrisCameraHostVdiImpl;
    auto *wrapper = reinterpret_cast<VdiWrapperCameraHost *>(vdiBase);
    auto *impl = new (std::nothrow) HybrisCameraHostVdiImpl();
    if (impl == nullptr) {
        CAMERA_VDI_LOGE("CreateCameraHostVdiInstance: out of memory");
        return HDF_ERR_MALLOC_FAIL;
    }
    impl->Init();
    wrapper->module = impl;
    return HDF_SUCCESS;
}

static int DestoryCameraHostVdiInstance(struct HdfVdiBase *vdiBase)
{
    using OHOS::VDI::Camera::V1_0::VdiWrapperCameraHost;
    using OHOS::Camera::Hybris::HybrisCameraHostVdiImpl;
    auto *wrapper = reinterpret_cast<VdiWrapperCameraHost *>(vdiBase);
    auto *impl = reinterpret_cast<HybrisCameraHostVdiImpl *>(wrapper->module);
    delete impl;
    wrapper->module = nullptr;
    return HDF_SUCCESS;
}

static struct OHOS::VDI::Camera::V1_0::VdiWrapperCameraHost g_vdiCameraHost = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "HybrisCameraHostVdi",
        .CreateVdiInstance = CreateCameraHostVdiInstance,
        .DestoryVdiInstance = DestoryCameraHostVdiInstance,
    },
    .module = nullptr,
};

} // extern "C"

HDF_VDI_INIT(g_vdiCameraHost);
