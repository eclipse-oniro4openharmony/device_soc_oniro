/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * N12.5.3b — ICameraProvider::getCameraIdList smoke test.
 *
 * Builds on N12.5.3a: resolves the camera provider via
 * hwservicemanager, then calls getCameraIdList through the resolved
 * handle to enumerate Halium-side cameras.  Expects 3 IDs on the
 * Volla X23 (rear-primary S5KGM1ST, front OV16A1Q, rear-wide
 * GC08A3WIDE).
 */

#include <cstdio>
#include <cstdlib>

#include "hidl/hw_binder_client.h"
#include "hidl/hw_camera_provider.h"
#include "hidl/hw_service_manager.h"
#include "hybris_camera_log.h"

using namespace OHOS::Camera::Hybris::Hidl;

int main(int argc, char **argv)
{
    const char *fqName = "android.hardware.camera.provider@2.6::ICameraProvider";
    const char *instance = "internal/0";
    if (argc >= 2) fqName = argv[1];
    if (argc >= 3) instance = argv[2];

    printf("hidl_camera_smoke — N12.5.3b\n");

    HwBinderClient client;
    if (client.Open() != HwBinderClient::Result::Ok) {
        fprintf(stderr, "HwBinderClient::Open failed\n");
        return 1;
    }

    HwServiceManager sm(&client);
    uint32_t handle = 0;
    bool isNull = false;
    if (!sm.GetService(fqName, instance, &handle, &isNull) || isNull) {
        fprintf(stderr, "GetService(%s/%s) failed or null\n", fqName, instance);
        return 2;
    }
    printf("Resolved %s/%s → handle=%u\n", fqName, instance, handle);

    HwCameraProvider provider(&client, handle);
    int32_t halStatus = -1;
    std::vector<std::string> cameraIds;
    if (!provider.GetCameraIdList(&halStatus, &cameraIds)) {
        fprintf(stderr, "GetCameraIdList failed\n");
        return 3;
    }
    printf("HAL Status = %d  (0 = OK)\n", halStatus);
    printf("Camera IDs (%zu):\n", cameraIds.size());
    for (size_t i = 0; i < cameraIds.size(); ++i) {
        printf("  [%zu] %s\n", i, cameraIds[i].c_str());
    }

    printf("hidl_camera_smoke: SUCCESS\n");
    HILOG_INFO(LOG_CORE,
               "hidl_camera_smoke: SUCCESS n=%{public}zu halStatus=%{public}d",
               cameraIds.size(), halStatus);
    return 0;
}
