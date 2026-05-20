/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hybris_camera_host_vdi_impl.h"

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

int32_t HybrisCameraHostVdiImpl::Init()
{
    std::lock_guard<std::mutex> lock(mutex_);
    /*
     * Match the HCS ability_01 template ("lcam001").  When N12.5 brings
     * up the libhybris HIDL ICameraProvider client, this will be
     * replaced by provider->getCameraIdList() output translated into
     * OHOS logical camera ids.
     */
    cameraIds_ = { "lcam001" };
    CAMERA_VDI_LOGI("VDI registered, cameraIds=%{public}s", cameraIds_[0].c_str());
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
