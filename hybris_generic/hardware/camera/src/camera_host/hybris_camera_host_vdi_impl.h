/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Hybris Camera Host VDI — implements ICameraHostVdi.
 *
 * Loaded by the OHOS camera HDF host (libcamera_host_service_1.0.z.so)
 * via HdfLoadVdi from the vdiLibList entry in
 *   vendor/oniro/hybris_generic/hdf_config/uhdf/camera/hdi_impl/camera_host_config.hcs
 *
 * Wiring under N12.D (droidmedia pivot — phase_n12_camera_droidmedia.md):
 *   - Init() drives the Droid::Loader bring-up (dlopen libdroidmedia.so
 *     + binder NS bind + _droid_media_init()) and populates
 *     cameraIds_ ("0", "1", "2") + perCameraInfo_ from
 *     droid_media_camera_get_info.
 *   - OpenCamera() calls Droid::Loader::Connect to obtain a
 *     DroidMediaCamera* and wraps it in HybrisCameraDeviceVdiImpl.
 *   - SetFlashlight stays METHOD_NOT_SUPPORTED — the on-device
 *     libdroidmedia.so revision lacks droid_media_camera_set_torch_mode.
 *
 * Falls back to the HCS "lcam001" placeholder if the loader can't bring
 * itself up (e.g. CAP_SYS_ADMIN missing → unshare(2) fails) so the HDF
 * host startup remains non-fatal during early boot / config drift.
 */

#ifndef HYBRIS_CAMERA_HOST_VDI_IMPL_H
#define HYBRIS_CAMERA_HOST_VDI_IMPL_H

#include <mutex>
#include <string>
#include <vector>

#include "droidmedia/droidmediacamera.h"
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

    /*
     * Indexed by OHOS cameraId ("0" → 0).  Populated by Init from
     * droid_media_camera_get_info — used by hybris_camera_ability.cpp
     * to fill OHOS_SENSOR_ORIENTATION and OHOS_LENS_FACING in the
     * static metadata blob without round-tripping per-call.
     */
    struct PerCameraInfo {
        int droidIndex   = -1;
        int facing       = -1;   /* DROID_MEDIA_CAMERA_FACING_{FRONT=0,BACK=1} */
        int orientation  = 0;    /* degrees, multiple of 90 */
    };
    const PerCameraInfo *FindInfo(const std::string &cameraId) const;

private:
    /*
     * Pull camera count + per-camera info via Droid::Loader.  Populates
     * cameraIds_ + perCameraInfo_; on failure leaves them empty so the
     * caller can fall back.
     */
    bool LoadCamerasFromDroidMedia();

    std::mutex                    mutex_;
    sptr<ICameraHostVdiCallback>  hostCallback_;
    std::vector<std::string>      cameraIds_;
    std::vector<PerCameraInfo>    perCameraInfo_;
    bool                          fallback_ = false;
};

} // namespace OHOS::Camera::Hybris

#endif
