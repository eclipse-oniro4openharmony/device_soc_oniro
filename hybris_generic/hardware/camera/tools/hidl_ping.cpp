/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * N12.5.2 milestone smoke — sanity-check the refactored
 * HwParcel/HwBinderClient/HwServiceManager modules by sending an
 * IBase::ping to handle 0 (hwservicemanager) and expecting EX_NONE.
 *
 * Replaces the inline 600-line spike that lived here in N12.5.2(a);
 * the wire-format archaeology that informed those modules is captured
 * in phase_n12_camera.md and src/hidl/hw_binder_abi.h.
 */

#include <cstdio>

#include "hidl/hw_binder_client.h"
#include "hidl/hw_service_manager.h"
#include "hybris_camera_log.h"

using namespace OHOS::Camera::Hybris::Hidl;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("hidl_ping — N12.5.2 (refactored)\n");

    HwBinderClient client;
    auto rc = client.Open("/dev/hwbinder");
    if (rc != HwBinderClient::Result::Ok) {
        fprintf(stderr, "HwBinderClient::Open: %s\n",
                HwBinderClient::ResultName(rc));
        return 1;
    }
    printf("opened /dev/hwbinder, protocol=8, mmap=256KB, BC_ENTER_LOOPER ok\n");

    HwServiceManager sm(&client);
    if (!sm.Ping()) {
        fprintf(stderr, "hidl_ping: FAILED\n");
        HILOG_ERROR(LOG_CORE, "hidl_ping: FAILED");
        return 2;
    }

    printf("hidl_ping: SUCCESS — EX_NONE round-trip on /dev/hwbinder\n");
    HILOG_INFO(LOG_CORE, "hidl_ping: SUCCESS");
    return 0;
}
