/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hw_camera_provider.h"

#include "hybris_camera_log.h"

namespace OHOS::Camera::Hybris::Hidl {

bool HwCameraProvider::GetCameraIdList(int32_t *outHalStatus,
                                       std::vector<std::string> *outCameraIds)
{
    *outHalStatus = -1;
    outCameraIds->clear();

    HwParcel request;
    /* getCameraIdList is a V2.4-defined method → V2.4 descriptor. */
    request.WriteInterfaceToken(kDescriptorV2_4);

    HwBinderClient::Reply reply;
    auto rc = client_->Transact(handle_, TRANSACTION_getCameraIdList,
                                request, &reply);
    if (rc != HwBinderClient::Result::Ok) {
        CAMERA_VDI_LOGE("HwCameraProvider::GetCameraIdList: transact "
                        "rc=%{public}s", HwBinderClient::ResultName(rc));
        return false;
    }

    auto reader = reply.Reader();
    if (reader.IsStatus()) {
        int32_t st = 0;
        reader.ReadInt32(&st);
        CAMERA_VDI_LOGE("HwCameraProvider::GetCameraIdList: status_t=%{public}d",
                        st);
        return false;
    }

    /*
     * Wire format of generates (Status, vec<string>) reply:
     *   int32  exception code (0 = EX_NONE)
     *   int32  camera HAL Status enum
     *   hidl_vec<hidl_string>  (2 + N buffer_objects, see Reader impl)
     */
    int32_t ex = 0;
    if (!reader.ReadInt32(&ex)) {
        CAMERA_VDI_LOGE("HwCameraProvider::GetCameraIdList: reply too short "
                        "for exception code");
        return false;
    }
    if (ex != 0) {
        CAMERA_VDI_LOGE("HwCameraProvider::GetCameraIdList: HIDL exception "
                        "code %{public}d", ex);
        return false;
    }

    if (!reader.ReadInt32(outHalStatus)) {
        CAMERA_VDI_LOGE("HwCameraProvider::GetCameraIdList: reply too short "
                        "for HAL Status");
        return false;
    }

    if (!reader.ReadHidlStringVec(outCameraIds)) {
        CAMERA_VDI_LOGE("HwCameraProvider::GetCameraIdList: failed to parse "
                        "hidl_vec<hidl_string> "
                        "(data_size=%{public}zu, halStatus=%{public}d)",
                        reader.DataSize(), *outHalStatus);
        return false;
    }
    return true;
}

bool HwCameraProvider::GetCameraDeviceInterface(
    const std::string &cameraDeviceName, int32_t *outHalStatus,
    uint32_t *outHandle, bool *outIsNull)
{
    *outHalStatus = -1;
    *outHandle    = 0;
    *outIsNull    = true;

    HwParcel request;
    /* getCameraDeviceInterface_V3_x is defined in V2.4 → V2.4 descriptor.
     * Confirmed via disassembly of _hidl_getCameraDeviceInterface_V3_x in
     * android.hardware.camera.provider@2.4.so (2026-05-21). */
    request.WriteInterfaceToken(kDescriptorV2_4);
    request.WriteHidlString(cameraDeviceName);

    HwBinderClient::Reply reply;
    auto rc = client_->Transact(handle_,
                                TRANSACTION_getCameraDeviceInterface_V3_x,
                                request, &reply);
    if (rc != HwBinderClient::Result::Ok) {
        CAMERA_VDI_LOGE("HwCameraProvider::GetCameraDeviceInterface(%{public}s)"
                        ": transact rc=%{public}s",
                        cameraDeviceName.c_str(),
                        HwBinderClient::ResultName(rc));
        return false;
    }

    auto reader = reply.Reader();
    if (reader.IsStatus()) {
        int32_t st = 0;
        reader.ReadInt32(&st);
        CAMERA_VDI_LOGE("HwCameraProvider::GetCameraDeviceInterface: "
                        "status_t=%{public}d", st);
        return false;
    }

    /*
     * Wire format of generates (Status, ICameraDevice) reply:
     *   int32  exception code (EX_NONE = 0)
     *   int32  camera HAL Status enum
     *   flat_binder_object  (24 bytes) for the returned sp<ICameraDevice>
     */
    int32_t ex = 0;
    if (!reader.ReadInt32(&ex)) {
        CAMERA_VDI_LOGE("GetCameraDeviceInterface: reply too short for "
                        "exception code");
        return false;
    }
    if (ex != 0) {
        CAMERA_VDI_LOGE("GetCameraDeviceInterface: HIDL exception %{public}d",
                        ex);
        return false;
    }
    if (!reader.ReadInt32(outHalStatus)) {
        CAMERA_VDI_LOGE("GetCameraDeviceInterface: reply too short for "
                        "HAL Status");
        return false;
    }
    if (*outHalStatus != 0) {
        CAMERA_VDI_LOGE("GetCameraDeviceInterface(%{public}s): "
                        "HAL Status=%{public}d (non-OK)",
                        cameraDeviceName.c_str(), *outHalStatus);
        return false;
    }
    if (!reader.ReadFlatBinder(outHandle, outIsNull)) {
        CAMERA_VDI_LOGE("GetCameraDeviceInterface: failed to read "
                        "flat_binder_object for ICameraDevice");
        return false;
    }
    return true;
}

} // namespace OHOS::Camera::Hybris::Hidl
