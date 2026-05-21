/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * HwCameraDeviceCallback — local-binder server impl of
 * android.hardware.camera.device@3.4::ICameraDeviceCallback.
 *
 * Halium acquires this via the `sp<ICameraDeviceCallback>` argument to
 * `ICameraDevice::open(callback)`, then BC_TRANSACTIONs back into our
 * process for `processCaptureResult` (V3.2 TX 1) and `notify` (V3.2 TX
 * 2).  V3.4 / V3.5 add `requestStreamBuffers` and `returnStreamBuffers`
 * for HAL buffer manager flows — handled with logs for now.
 *
 * In addition to the camera-specific methods, every BnHwBase responds
 * to the inherited IBase methods (interfaceDescriptor, interfaceChain,
 * ping, getDebugInfo, linkToDeath/unlinkToDeath,
 * notifySyspropsChanged, getHashChain, setHALInstrumentation, debug).
 * Halium normally calls only interfaceDescriptor + ping at registration
 * time so we stub the rest minimally.
 *
 * Bring-up scope (N12.6.1): log + ack incoming results.  The actual
 * frame-delivery handoff (parse CaptureResult, marshal to OHOS
 * IStreamOperatorVdiCallback + the BufferProducerSequenceable queue)
 * lands in N12.7 once buffer interop is wired.
 */

#ifndef HYBRIS_HIDL_HW_CAMERA_DEVICE_CALLBACK_H
#define HYBRIS_HIDL_HW_CAMERA_DEVICE_CALLBACK_H

#include "hw_binder_server.h"

namespace OHOS::Camera::Hybris::Hidl {

class HwCameraDeviceCallback : public LocalBinder {
public:
    /*
     * Per-interface TRANSACTION codes for ICameraDeviceCallback.
     * Confirmed by disassembling
     * BpHwCameraDeviceCallback::_hidl_processCaptureResult in
     * android.hardware.camera.device@3.2.so on the X23.
     */
    enum : uint32_t {
        TRANSACTION_processCaptureResult       = 1,
        TRANSACTION_notify                     = 2,
        TRANSACTION_requestStreamBuffers       = 3,
        TRANSACTION_returnStreamBuffers        = 4,
        TRANSACTION_processCaptureResult_2_6   = 5,
    };

    static constexpr const char *kDescriptorV3_2 =
        "android.hardware.camera.device@3.2::ICameraDeviceCallback";
    static constexpr const char *kDescriptorV3_4 =
        "android.hardware.camera.device@3.4::ICameraDeviceCallback";
    static constexpr const char *kDescriptorV3_5 =
        "android.hardware.camera.device@3.5::ICameraDeviceCallback";

    int32_t OnTransact(uint32_t code, HwParcel::Reader &data,
                       HwParcel *reply) override;

private:
    int32_t HandleIBaseMethod(uint32_t code, HwParcel::Reader &data,
                              HwParcel *reply);
};

} // namespace OHOS::Camera::Hybris::Hidl

#endif // HYBRIS_HIDL_HW_CAMERA_DEVICE_CALLBACK_H
