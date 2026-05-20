/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Hybris Camera Host VDI — implements ICameraHostVdi.
 *
 * The OHOS camera HDF host (libcamera_host_service_1.0.z.so) reads
 * vdiLibList from camera_host_config.hcs (currently
 * vendor/oniro/hybris_generic/hdf_config/uhdf/camera/hdi_impl/camera_host_config.hcs)
 * and dlopens libcamera_host_vdi_impl_1.0.z.so.  HDF then resolves the
 * exported `g_vdiCameraHost` VdiWrapperCameraHost via HDF_VDI_INIT, calls
 * its CreateVdiInstance which constructs HybrisCameraHostVdiImpl and
 * stores the ICameraHostVdi* in vdiWrapperCameraHost->module.
 *
 * Current state — scaffolding only (N12.4):
 *   - GetCameraIds returns a single "lcam001" matching the existing HCS
 *     ability_01 template.
 *   - GetCameraAbility returns NO_ERROR with an empty blob (HCS supplies
 *     static ability metadata to the host service directly).
 *   - OpenCamera / SetFlashlight / CloseAllCameras return
 *     METHOD_NOT_SUPPORTED.
 *
 * N12.5+ will replace these with calls into the libhybris-loaded
 * android.hardware.camera.provider@2.6::ICameraProvider — see
 * device/board/oniro/docs/hybris_generic/phase_n12_camera.md.
 */

#ifndef HYBRIS_CAMERA_HOST_VDI_IMPL_H
#define HYBRIS_CAMERA_HOST_VDI_IMPL_H

#include <mutex>
#include <string>
#include <vector>

#include "v1_0/icamera_host_vdi.h"

namespace OHOS::Camera::Hybris {

using OHOS::VDI::Camera::V1_0::ICameraHostVdi;
using OHOS::VDI::Camera::V1_0::ICameraHostVdiCallback;
using OHOS::VDI::Camera::V1_0::ICameraDeviceVdi;
using OHOS::VDI::Camera::V1_0::ICameraDeviceVdiCallback;

class HybrisCameraHostVdiImpl : public ICameraHostVdi {
public:
    HybrisCameraHostVdiImpl();
    ~HybrisCameraHostVdiImpl() override;

    int32_t Init();

    int32_t SetCallback(const sptr<ICameraHostVdiCallback> &callbackObj) override;
    int32_t GetCameraIds(std::vector<std::string> &cameraIds) override;
    int32_t GetCameraAbility(const std::string &cameraId,
                             std::vector<uint8_t> &cameraAbility) override;
    int32_t OpenCamera(const std::string &cameraId,
                       const sptr<ICameraDeviceVdiCallback> &callbackObj,
                       sptr<ICameraDeviceVdi> &device) override;
    int32_t SetFlashlight(const std::string &cameraId, bool isEnable) override;
    int32_t CloseAllCameras() override;

private:
    std::mutex                    mutex_;
    sptr<ICameraHostVdiCallback>  hostCallback_;
    std::vector<std::string>      cameraIds_;
};

} // namespace OHOS::Camera::Hybris

#endif
