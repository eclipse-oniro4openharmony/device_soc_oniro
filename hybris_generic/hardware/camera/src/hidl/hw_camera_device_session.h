/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * HwCameraDeviceSession — pure-musl proxy for Halium's
 * android.hardware.camera.device@3.4::ICameraDeviceSession.
 *
 * Wraps the binder handle returned by HwCameraDevice::Open.
 *
 * ICameraDeviceSession TRANSACTION codes (verified by disassembly of
 * BpHwCameraDeviceSession in android.hardware.camera.device@3.{2,4}.so,
 * 2026-05-21):
 *
 *   V3.2 base:
 *     1 constructDefaultRequestSettings
 *     2 configureStreams
 *     3 processCaptureRequest
 *     4 getCaptureRequestMetadataQueue
 *     5 getCaptureResultMetadataQueue
 *     6 flush
 *     7 close
 *
 *   V3.4 extension:
 *     8 configureStreams_3_3
 *     9 configureStreams_3_4
 *    10 processCaptureRequest_3_4
 *
 * The `RequestTemplate` enum used by constructDefaultRequestSettings
 * (per `android.hardware.camera.device@3.2::RequestTemplate`):
 *   1 PREVIEW
 *   2 STILL_CAPTURE
 *   3 VIDEO_RECORD
 *   4 VIDEO_SNAPSHOT
 *   5 ZERO_SHUTTER_LAG
 *   6 MANUAL
 */

#ifndef HYBRIS_HIDL_HW_CAMERA_DEVICE_SESSION_H
#define HYBRIS_HIDL_HW_CAMERA_DEVICE_SESSION_H

#include <cstdint>
#include <vector>

#include "hw_binder_client.h"

namespace OHOS::Camera::Hybris::Hidl {

class HwCameraDeviceSession {
public:
    HwCameraDeviceSession(HwBinderClient *client, uint32_t handle)
        : client_(client), handle_(handle) {}

    enum : uint32_t {
        TRANSACTION_constructDefaultRequestSettings = 1,
        TRANSACTION_configureStreams                = 2,
        TRANSACTION_processCaptureRequest           = 3,
        TRANSACTION_getCaptureRequestMetadataQueue  = 4,
        TRANSACTION_getCaptureResultMetadataQueue   = 5,
        TRANSACTION_flush                           = 6,
        TRANSACTION_close                           = 7,
        TRANSACTION_configureStreams_3_3            = 8,
        TRANSACTION_configureStreams_3_4            = 9,
        TRANSACTION_processCaptureRequest_3_4       = 10,
    };

    enum RequestTemplate : uint32_t {
        TEMPLATE_PREVIEW            = 1,
        TEMPLATE_STILL_CAPTURE      = 2,
        TEMPLATE_VIDEO_RECORD       = 3,
        TEMPLATE_VIDEO_SNAPSHOT     = 4,
        TEMPLATE_ZERO_SHUTTER_LAG   = 5,
        TEMPLATE_MANUAL             = 6,
    };

    static constexpr const char *kDescriptorV3_2 =
        "android.hardware.camera.device@3.2::ICameraDeviceSession";
    static constexpr const char *kDescriptorV3_4 =
        "android.hardware.camera.device@3.4::ICameraDeviceSession";

    /*
     * constructDefaultRequestSettings(RequestTemplate type) generates
     *   (Status, CameraMetadata template)
     *
     * `CameraMetadata` is `hidl_vec<uint8_t>` of the Android
     * `camera_metadata_t` blob.  Lets us check the session is alive
     * without serialising the StreamConfiguration / FMQ surface area.
     */
    bool ConstructDefaultRequestSettings(RequestTemplate type,
                                         int32_t *outHalStatus,
                                         std::vector<uint8_t> *outBlob);

    uint32_t Handle() const { return handle_; }

private:
    HwBinderClient *client_;
    uint32_t        handle_;
};

} // namespace OHOS::Camera::Hybris::Hidl

#endif // HYBRIS_HIDL_HW_CAMERA_DEVICE_SESSION_H
