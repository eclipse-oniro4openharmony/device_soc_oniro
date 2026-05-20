/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * HwCameraProvider — pure-musl proxy for Halium's
 * android.hardware.camera.provider@2.6::ICameraProvider over /dev/hwbinder.
 *
 * Used by the OHOS camera VDI (libcamera_host_vdi_impl) to enumerate
 * Halium-side cameras and forward camera-device requests without ever
 * loading bionic or libhybris into the OHOS process.
 *
 * ICameraProvider transaction codes use the standard hidl-gen
 * FIRST_CALL_TRANSACTION (1) + N pattern:
 *   1 setCallback
 *   2 getVendorTags
 *   3 getCameraIdList            ← used here
 *   4 isSetTorchModeSupported
 *   5 getCameraDeviceInterface_V1_x
 *   6 getCameraDeviceInterface_V3_x
 * (V2_5 adds 7=notifyDeviceStateChange; V2_6 adds 8=getConcurrentStreamingCameraIds,
 *  9=isConcurrentStreamConfigurationSupported, 10=getCameraDeviceInterface_V3_6,
 *  …).  Confirmed for V2_4 by disassembling
 * BnHwCameraProvider::onTransact in android.hardware.camera.provider@2.4.so
 * on the X23 (2026-05-21).
 *
 * Camera HAL Status enum (from android.hardware.camera.common@1.0):
 *   0 OK, 1 ILLEGAL_ARGUMENT, 2 CAMERA_IN_USE, 3 MAX_CAMERAS_IN_USE,
 *   4 METHOD_NOT_SUPPORTED, 5 OPERATION_NOT_SUPPORTED, 6 CAMERA_DISCONNECTED,
 *   7 INTERNAL_ERROR.
 */

#ifndef HYBRIS_HIDL_HW_CAMERA_PROVIDER_H
#define HYBRIS_HIDL_HW_CAMERA_PROVIDER_H

#include <cstdint>
#include <string>
#include <vector>

#include "hw_binder_client.h"

namespace OHOS::Camera::Hybris::Hidl {

class HwCameraProvider {
public:
    /*
     * Take ownership of an already-resolved+acquired binder handle
     * (obtained via HwServiceManager::GetService).  Caller is
     * responsible for not destroying the underlying HwBinderClient
     * before this HwCameraProvider goes out of scope.
     */
    HwCameraProvider(HwBinderClient *client, uint32_t handle)
        : client_(client), handle_(handle) {}

    /*
     * ICameraProvider::getCameraIdList() generates (Status, vec<string>)
     *
     * Returns the list of camera device IDs registered with the HAL
     * (e.g. ["0", "1", "2"] for X23: rear-primary + front + rear-wide).
     * `outHalStatus` is set to the camera HAL Status (0 = OK).
     */
    bool GetCameraIdList(int32_t *outHalStatus,
                         std::vector<std::string> *outCameraIds);

    enum : uint32_t {
        TRANSACTION_setCallback                          = 1,
        TRANSACTION_getVendorTags                        = 2,
        TRANSACTION_getCameraIdList                      = 3,
        TRANSACTION_isSetTorchModeSupported              = 4,
        TRANSACTION_getCameraDeviceInterface_V1_x        = 5,
        TRANSACTION_getCameraDeviceInterface_V3_x        = 6,
    };

    /*
     * Descriptors.  The descriptor enforced by enforceInterface is the
     * one belonging to the BnHwCameraProvider class that *defined* the
     * method — NOT the most-derived class.  So:
     *   - V2.4 methods (codes 1–6, incl. getCameraIdList) → V2.4 descriptor
     *   - V2.5 methods (notifyDeviceStateChange)          → V2.5 descriptor
     *   - V2.6 methods (getCameraDeviceInterface_V3_6, …) → V2.6 descriptor
     * Verified by disassembling _hidl_getCameraIdList in
     * android.hardware.camera.provider@2.4.so: it loads the V2.4
     * descriptor string from its own GOT before enforceInterface.
     */
    static constexpr const char *kDescriptorV2_4 =
        "android.hardware.camera.provider@2.4::ICameraProvider";
    static constexpr const char *kDescriptorV2_5 =
        "android.hardware.camera.provider@2.5::ICameraProvider";
    static constexpr const char *kDescriptorV2_6 =
        "android.hardware.camera.provider@2.6::ICameraProvider";

    uint32_t Handle() const { return handle_; }

private:
    HwBinderClient *client_;
    uint32_t        handle_;
};

} // namespace OHOS::Camera::Hybris::Hidl

#endif // HYBRIS_HIDL_HW_CAMERA_PROVIDER_H
