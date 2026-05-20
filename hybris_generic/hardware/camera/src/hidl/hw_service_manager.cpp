/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hw_service_manager.h"

#include "hybris_camera_log.h"

namespace OHOS::Camera::Hybris::Hidl {

bool HwServiceManager::EnsureAcquired()
{
    if (smHandle_.Valid()) return true;
    smHandle_ = client_->Acquire(0);
    return smHandle_.Valid();
}

bool HwServiceManager::Ping(const char *interfaceDescriptor)
{
    if (!EnsureAcquired()) {
        CAMERA_VDI_LOGE("HwServiceManager: failed to acquire handle 0");
        return false;
    }

    HwParcel request;
    request.WriteInterfaceToken(interfaceDescriptor);

    HwBinderClient::Reply reply;
    auto rc = client_->Transact(0,
                                HidlBase::TRANSACTION_ping,
                                request, &reply);
    if (rc != HwBinderClient::Result::Ok) {
        CAMERA_VDI_LOGE("HwServiceManager::Ping: transact rc=%{public}s",
                        HwBinderClient::ResultName(rc));
        return false;
    }

    /*
     * Successful HIDL reply has flags=0 and first int32 = HIDL Status
     * (0 = EX_NONE).  TF_STATUS_CODE replies signal a bare status_t
     * failure (e.g. enforceInterface mismatch returns INT32_MIN+1).
     */
    auto reader = reply.Reader();
    if (reader.IsStatus()) {
        int32_t st = 0;
        reader.ReadInt32(&st);
        CAMERA_VDI_LOGE("HwServiceManager::Ping: receiver returned "
                        "status_t=%{public}d (flags=0x%{public}x)",
                        st, reader.Flags());
        return false;
    }

    int32_t ex = 0;
    if (!reader.ReadInt32(&ex)) {
        CAMERA_VDI_LOGE("HwServiceManager::Ping: reply too short "
                        "(data_size=%{public}zu)", reader.DataSize());
        return false;
    }
    if (ex != 0) {
        CAMERA_VDI_LOGE("HwServiceManager::Ping: HIDL exception code "
                        "%{public}d", ex);
        return false;
    }
    return true;
}

bool HwServiceManager::GetService(const std::string &fqName,
                                  const std::string &instance,
                                  uint32_t *outHandle, bool *outIsNull)
{
    *outHandle = 0;
    *outIsNull = false;

    if (!EnsureAcquired()) {
        CAMERA_VDI_LOGE("HwServiceManager: failed to acquire handle 0");
        return false;
    }

    /*
     * Wire format for IServiceManager::get(fqName, name):
     *   - writeInterfaceToken("android.hidl.manager@1.0::IServiceManager")
     *   - writeHidlString(fqName)
     *   - writeHidlString(instance)
     *
     * WriteInterfaceToken is required by enforceInterface inside
     * BnHwServiceManager::_hidl_get; using IBase's descriptor here
     * returns -EBADMSG (status_t = -74) because _hidl_get does its
     * own enforceInterface check before dispatching.
     */
    HwParcel request;
    request.WriteInterfaceToken(kDescriptor);
    request.WriteHidlString(fqName);
    request.WriteHidlString(instance);

    HwBinderClient::Reply reply;
    auto rc = client_->Transact(0, TRANSACTION_get, request, &reply);
    if (rc != HwBinderClient::Result::Ok) {
        CAMERA_VDI_LOGE("HwServiceManager::GetService(%{public}s/%{public}s): "
                        "transact rc=%{public}s",
                        fqName.c_str(), instance.c_str(),
                        HwBinderClient::ResultName(rc));
        return false;
    }

    auto reader = reply.Reader();
    if (reader.IsStatus()) {
        int32_t st = 0;
        reader.ReadInt32(&st);
        CAMERA_VDI_LOGE("HwServiceManager::GetService: status_t=%{public}d "
                        "(flags=0x%{public}x)", st, reader.Flags());
        return false;
    }

    int32_t ex = 0;
    if (!reader.ReadInt32(&ex)) {
        CAMERA_VDI_LOGE("HwServiceManager::GetService: reply too short");
        return false;
    }
    if (ex != 0) {
        CAMERA_VDI_LOGE("HwServiceManager::GetService: HIDL exception "
                        "code %{public}d", ex);
        return false;
    }

    /*
     * Reply layout after EX_NONE int32: flat_binder_object (24 bytes).
     * Either BINDER_TYPE_HANDLE with a real handle, or hdr.type==0
     * (or BINDER_TYPE_BINDER) with handle==0/binder==0 for null
     * (service not registered).
     */
    if (!reader.ReadFlatBinder(outHandle, outIsNull)) {
        CAMERA_VDI_LOGE("HwServiceManager::GetService: failed to read "
                        "flat_binder_object (data_size=%{public}zu)",
                        reader.DataSize());
        return false;
    }
    return true;
}

} // namespace OHOS::Camera::Hybris::Hidl
