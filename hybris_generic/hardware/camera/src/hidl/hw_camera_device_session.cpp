/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hw_camera_device_session.h"

#include "hybris_camera_log.h"

namespace OHOS::Camera::Hybris::Hidl {

bool HwCameraDeviceSession::ConstructDefaultRequestSettings(
    RequestTemplate type, int32_t *outHalStatus,
    std::vector<uint8_t> *outBlob)
{
    *outHalStatus = -1;
    outBlob->clear();

    HwParcel request;
    /* constructDefaultRequestSettings is defined in V3.2. */
    request.WriteInterfaceToken(kDescriptorV3_2);
    request.WriteUint32(static_cast<uint32_t>(type));

    HwBinderClient::Reply reply;
    auto rc = client_->Transact(handle_,
                                TRANSACTION_constructDefaultRequestSettings,
                                request, &reply);
    if (rc != HwBinderClient::Result::Ok) {
        CAMERA_VDI_LOGE("ConstructDefaultRequestSettings: transact "
                        "rc=%{public}s", HwBinderClient::ResultName(rc));
        return false;
    }

    auto reader = reply.Reader();
    if (reader.IsStatus()) {
        int32_t st = 0;
        reader.ReadInt32(&st);
        CAMERA_VDI_LOGE("ConstructDefaultRequestSettings: status_t=%{public}d",
                        st);
        return false;
    }

    int32_t ex = 0;
    if (!reader.ReadInt32(&ex) || ex != 0) {
        CAMERA_VDI_LOGE("ConstructDefaultRequestSettings: ex=%{public}d", ex);
        return false;
    }
    if (!reader.ReadInt32(outHalStatus) || *outHalStatus != 0) {
        CAMERA_VDI_LOGE("ConstructDefaultRequestSettings: hal=%{public}d",
                        *outHalStatus);
        return false;
    }
    if (!reader.ReadHidlVecUint8(outBlob)) {
        CAMERA_VDI_LOGE("ConstructDefaultRequestSettings: vec parse failed");
        return false;
    }
    CAMERA_VDI_LOGI("ConstructDefaultRequestSettings(type=%{public}u): "
                    "%{public}zu bytes",
                    (uint32_t)type, outBlob->size());
    return true;
}

} // namespace OHOS::Camera::Hybris::Hidl
