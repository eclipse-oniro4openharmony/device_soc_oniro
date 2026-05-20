/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hybris_camera_host_vdi_impl.h"

#include <sstream>

#include "hidl/hw_binder_client.h"
#include "hidl/hw_camera_provider.h"
#include "hidl/hw_service_manager.h"
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

bool HybrisCameraHostVdiImpl::LoadCameraIdsFromHalium()
{
    auto client = std::make_unique<Hidl::HwBinderClient>();
    auto rc = client->Open("/dev/hwbinder");
    if (rc != Hidl::HwBinderClient::Result::Ok) {
        CAMERA_VDI_LOGE("LoadCameraIdsFromHalium: HwBinderClient::Open "
                        "failed (%{public}s)",
                        Hidl::HwBinderClient::ResultName(rc));
        return false;
    }

    Hidl::HwServiceManager sm(client.get());
    uint32_t handle = 0;
    bool isNull = false;
    constexpr const char *kProviderFq =
        "android.hardware.camera.provider@2.6::ICameraProvider";
    constexpr const char *kProviderInstance = "internal/0";
    if (!sm.GetService(kProviderFq, kProviderInstance, &handle, &isNull) ||
        isNull) {
        CAMERA_VDI_LOGE("LoadCameraIdsFromHalium: GetService(%{public}s/%{public}s) "
                        "failed (isNull=%{public}d)",
                        kProviderFq, kProviderInstance, (int)isNull);
        return false;
    }

    auto provider = std::make_unique<Hidl::HwCameraProvider>(client.get(),
                                                              handle);
    int32_t halStatus = -1;
    std::vector<std::string> ids;
    if (!provider->GetCameraIdList(&halStatus, &ids)) {
        CAMERA_VDI_LOGE("LoadCameraIdsFromHalium: GetCameraIdList failed "
                        "(halStatus=%{public}d)", halStatus);
        return false;
    }
    if (halStatus != 0) {
        CAMERA_VDI_LOGE("LoadCameraIdsFromHalium: HAL Status=%{public}d "
                        "(non-OK)", halStatus);
        return false;
    }

    /*
     * Halium camera IDs come back fully-qualified, e.g.
     *   "device@3.6/internal/0"
     * For OHOS-side cameraId values we use the trailing token after
     * the last '/' — the bare per-device instance name ("internal/0"
     * → "0").  OpenCamera maps this back to the FQ name when calling
     * IServiceManager::get again for the camera device proxy.
     */
    cameraIds_.clear();
    cameraIds_.reserve(ids.size());
    for (const auto &fq : ids) {
        size_t slash = fq.find_last_of('/');
        cameraIds_.push_back(slash == std::string::npos
                                 ? fq : fq.substr(slash + 1));
    }

    std::ostringstream summary;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) summary << ",";
        summary << ids[i] << "→" << cameraIds_[i];
    }
    CAMERA_VDI_LOGI("LoadCameraIdsFromHalium: %{public}zu cameras: %{public}s",
                    cameraIds_.size(), summary.str().c_str());

    hwClient_   = std::move(client);
    hwProvider_ = std::move(provider);
    return true;
}

int32_t HybrisCameraHostVdiImpl::Init()
{
    std::lock_guard<std::mutex> lock(mutex_);

    /*
     * Bridge to Halium for the real list.  Falls back to the HCS
     * "lcam001" placeholder if the libhybris transport can't reach
     * Halium — keeps the HDF host startup non-fatal during early
     * boot / pre-camerahalserver-ready windows.
     */
    if (!LoadCameraIdsFromHalium()) {
        cameraIds_ = { "lcam001" };
        fallback_ = true;
        CAMERA_VDI_LOGW("VDI registered with fallback cameraIds=%{public}s",
                        cameraIds_[0].c_str());
        return VDI::Camera::V1_0::NO_ERROR;
    }

    CAMERA_VDI_LOGI("VDI registered, %{public}zu Halium cameras",
                    cameraIds_.size());
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHostVdiImpl::SetCallback(const sptr<ICameraHostVdiCallback> &callbackObj)
{
    if (callbackObj == nullptr) {
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }
    /*
     * Bind death recipient via the base-class helper.  When the OHOS
     * framework callback dies (camera_service crash), the base class
     * fires CloseAllCameras for us.
     */
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
     * HCS supplies the static ability blob to the host service via the
     * `ability_01` template (camera_host_config.hcs).  The VDI is only
     * consulted when the host service needs a runtime override; for the
     * stub we always defer to HCS.  Returning NO_ERROR with an empty
     * blob causes the host service to fall back to HCS metadata.
     */
    (void)cameraId;
    cameraAbility.clear();
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisCameraHostVdiImpl::OpenCamera(const std::string &cameraId,
                                            const sptr<ICameraDeviceVdiCallback> &callbackObj,
                                            sptr<ICameraDeviceVdi> &device)
{
    (void)cameraId;
    (void)callbackObj;
    device = nullptr;
    CAMERA_VDI_LOGW("OpenCamera(%{public}s) — N12.4 stub, real bridging in N12.7", cameraId.c_str());
    return VDI::Camera::V1_0::METHOD_NOT_SUPPORTED;
}

int32_t HybrisCameraHostVdiImpl::SetFlashlight(const std::string &cameraId, bool isEnable)
{
    (void)cameraId;
    (void)isEnable;
    CAMERA_VDI_LOGW("SetFlashlight — N12.4 stub, real bridging in N12.12");
    return VDI::Camera::V1_0::METHOD_NOT_SUPPORTED;
}

int32_t HybrisCameraHostVdiImpl::CloseAllCameras()
{
    CAMERA_VDI_LOGI("CloseAllCameras — N12.4 stub no-op");
    return VDI::Camera::V1_0::NO_ERROR;
}

} // namespace OHOS::Camera::Hybris

/* -------------------------------------------------------------------------
 * HDF VDI plumbing — exported so libcamera_host_service can dlopen + bind.
 * -------------------------------------------------------------------------
 * The OHOS camera HDF host (libcamera_host_service_1.0.z.so) calls
 * HdfLoadVdi(libname) on each entry in vdiLibList.  HdfLoadVdi expects
 * a global `g_vdi` of HdfVdiBase type with CreateVdiInstance /
 * DestoryVdiInstance hooks, exported via HDF_VDI_INIT(...).
 *
 * Pattern matches the v4l2 reference VDI:
 *   drivers/peripheral/camera/vdi_base/v4l2/src/camera_host/camera_host_vdi_impl.cpp
 * The exported symbol name (g_vdi via HDF_VDI_INIT macro) is what
 * HdfLoadVdi looks up via dlsym after dlopen.
 */
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
