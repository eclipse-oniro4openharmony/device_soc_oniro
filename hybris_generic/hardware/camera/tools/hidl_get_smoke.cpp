/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * N12.5.3a — IServiceManager::get() smoke test.
 *
 * Resolves a HIDL service name to a strong binder handle via
 * hwservicemanager, then pings it via IBase::ping to prove the
 * resolved binder is alive end-to-end.
 *
 * Default target:
 *   android.hardware.camera.provider@2.6::ICameraProvider/internal/0
 * (the MediaTek X23 manifest at
 *  /android/vendor/etc/vintf/manifest/manifest_cameraprovider.xml
 *  registers it under "internal/0", NOT the more common "legacy/0".)
 *
 * Usage:  hidl_get_smoke [fqName] [instance]
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hidl/hw_binder_client.h"
#include "hidl/hw_service_manager.h"
#include "hybris_camera_log.h"

using namespace OHOS::Camera::Hybris::Hidl;

namespace {

bool PingHandle(HwBinderClient &client, uint32_t handle)
{
    /*
     * IBase::_hidl_ping always enforceInterfaces against the IBase
     * descriptor regardless of which concrete BnHwService dispatches
     * it.  Verified by libhidlbase.so disassembly in N12.5.2.
     */
    HwParcel request;
    request.WriteInterfaceToken("android.hidl.base@1.0::IBase");
    HwBinderClient::Reply reply;
    auto rc = client.Transact(handle,
                              HidlBase::TRANSACTION_ping, request, &reply);
    if (rc != HwBinderClient::Result::Ok) {
        fprintf(stderr, "ping(handle=%u) transact rc=%s\n",
                handle, HwBinderClient::ResultName(rc));
        return false;
    }
    auto reader = reply.Reader();
    if (reader.IsStatus()) {
        int32_t st = 0;
        reader.ReadInt32(&st);
        fprintf(stderr, "ping(handle=%u) status_t=%d (flags=0x%x)\n",
                handle, st, reader.Flags());
        return false;
    }
    int32_t ex = 0;
    if (!reader.ReadInt32(&ex) || ex != 0) {
        fprintf(stderr, "ping(handle=%u) HIDL exception=%d\n", handle, ex);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    const char *fqName = "android.hardware.camera.provider@2.6::ICameraProvider";
    const char *instance = "internal/0";
    if (argc >= 2) fqName = argv[1];
    if (argc >= 3) instance = argv[2];

    printf("hidl_get_smoke — N12.5.3a\n");
    printf("  fqName   = %s\n", fqName);
    printf("  instance = %s\n", instance);

    HwBinderClient client;
    auto rc = client.Open();
    if (rc != HwBinderClient::Result::Ok) {
        fprintf(stderr, "HwBinderClient::Open: %s\n",
                HwBinderClient::ResultName(rc));
        return 1;
    }

    HwServiceManager sm(&client);

    uint32_t handle = 0;
    bool isNull = false;
    if (!sm.GetService(fqName, instance, &handle, &isNull)) {
        fprintf(stderr, "GetService failed\n");
        return 2;
    }
    if (isNull) {
        fprintf(stderr, "GetService returned null — %s/%s not registered\n",
                fqName, instance);
        return 3;
    }

    printf("GetService → handle=%u\n", handle);

    if (!PingHandle(client, handle)) {
        fprintf(stderr, "ping on resolved handle failed\n");
        return 4;
    }

    printf("hidl_get_smoke: SUCCESS — resolved + pinged %s/%s (handle=%u)\n",
           fqName, instance, handle);
    HILOG_INFO(LOG_CORE, "hidl_get_smoke: SUCCESS handle=%{public}u", handle);
    return 0;
}
