/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hw_camera_device.h"

#include "hybris_camera_log.h"

namespace OHOS::Camera::Hybris::Hidl {

bool HwCameraDevice::GetCameraCharacteristics(int32_t *outHalStatus,
                                              std::vector<uint8_t> *outBlob)
{
    *outHalStatus = -1;
    outBlob->clear();

    HwParcel request;
    /* V3.2-defined method → V3.2 descriptor. */
    request.WriteInterfaceToken(kDescriptorV3_2);

    HwBinderClient::Reply reply;
    auto rc = client_->Transact(handle_,
                                TRANSACTION_getCameraCharacteristics,
                                request, &reply);
    if (rc != HwBinderClient::Result::Ok) {
        CAMERA_VDI_LOGE("HwCameraDevice::GetCameraCharacteristics: transact "
                        "rc=%{public}s", HwBinderClient::ResultName(rc));
        return false;
    }

    auto reader = reply.Reader();
    if (reader.IsStatus()) {
        int32_t st = 0;
        reader.ReadInt32(&st);
        CAMERA_VDI_LOGE("HwCameraDevice::GetCameraCharacteristics: "
                        "status_t=%{public}d", st);
        return false;
    }

    int32_t ex = 0;
    if (!reader.ReadInt32(&ex)) {
        CAMERA_VDI_LOGE("GetCameraCharacteristics: reply too short for "
                        "exception code");
        return false;
    }
    if (ex != 0) {
        CAMERA_VDI_LOGE("GetCameraCharacteristics: HIDL exception %{public}d",
                        ex);
        return false;
    }
    if (!reader.ReadInt32(outHalStatus)) {
        CAMERA_VDI_LOGE("GetCameraCharacteristics: reply too short for "
                        "HAL Status");
        return false;
    }
    if (*outHalStatus != 0) {
        CAMERA_VDI_LOGE("GetCameraCharacteristics: HAL Status=%{public}d "
                        "(non-OK)", *outHalStatus);
        return false;
    }
    if (!reader.ReadHidlVecUint8(outBlob)) {
        CAMERA_VDI_LOGE("GetCameraCharacteristics: failed to parse "
                        "hidl_vec<uint8_t>");
        return false;
    }
    CAMERA_VDI_LOGI("GetCameraCharacteristics: %{public}zu bytes",
                    outBlob->size());
    return true;
}

} // namespace OHOS::Camera::Hybris::Hidl
