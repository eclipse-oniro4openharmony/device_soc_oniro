/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * HwCameraDevice — pure-musl proxy for Halium's
 * android.hardware.camera.device@3.x::ICameraDevice over /dev/hwbinder.
 *
 * ICameraDevice@3.2 transaction codes (confirmed by disassembly of
 * BpHwCameraDevice in android.hardware.camera.device@3.2.so, 2026-05-21):
 *   1  getResourceCost
 *   2  getCameraCharacteristics
 *   3  setTorchMode
 *   4  open
 *   5  dumpState
 *
 * V3.4–V3.6 only add new methods; the V3.2 codes above keep their slots.
 *
 * Camera HAL Status enum (android.hardware.camera.common@1.0):
 *   0 OK, 1 ILLEGAL_ARGUMENT, 2 CAMERA_IN_USE, 3 MAX_CAMERAS_IN_USE,
 *   4 METHOD_NOT_SUPPORTED, 5 OPERATION_NOT_SUPPORTED, 6 CAMERA_DISCONNECTED,
 *   7 INTERNAL_ERROR.
 */

#ifndef HYBRIS_HIDL_HW_CAMERA_DEVICE_H
#define HYBRIS_HIDL_HW_CAMERA_DEVICE_H

#include <cstdint>
#include <string>
#include <vector>

#include "hw_binder_client.h"

namespace OHOS::Camera::Hybris::Hidl {

class HwCameraDevice {
public:
    HwCameraDevice(HwBinderClient *client, uint32_t handle)
        : client_(client), handle_(handle) {}

    /*
     * ICameraDevice::getCameraCharacteristics() generates (Status, CameraMetadata)
     *
     * `CameraMetadata` is a `hidl_vec<uint8_t>` of the Android
     * `camera_metadata_t` blob.  Copies the bytes into `outBlob`.
     */
    bool GetCameraCharacteristics(int32_t *outHalStatus,
                                  std::vector<uint8_t> *outBlob);

    enum : uint32_t {
        TRANSACTION_getResourceCost            = 1,
        TRANSACTION_getCameraCharacteristics   = 2,
        TRANSACTION_setTorchMode               = 3,
        TRANSACTION_open                       = 4,
        TRANSACTION_dumpState                  = 5,
    };

    /*
     * As with ICameraProvider: enforceInterface uses the *defining*
     * class's descriptor.  V3.2 defines all the methods listed above,
     * so they all use the V3.2 descriptor even when the registered
     * service implements V3.4 / V3.5 / V3.6.
     */
    static constexpr const char *kDescriptorV3_2 =
        "android.hardware.camera.device@3.2::ICameraDevice";

    uint32_t Handle() const { return handle_; }

private:
    HwBinderClient *client_;
    uint32_t        handle_;
};

} // namespace OHOS::Camera::Hybris::Hidl

#endif // HYBRIS_HIDL_HW_CAMERA_DEVICE_H
