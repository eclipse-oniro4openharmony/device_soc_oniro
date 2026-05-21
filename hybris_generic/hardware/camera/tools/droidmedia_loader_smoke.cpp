/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * N12.D.3 — exercise the Droid::Loader singleton end-to-end.
 *
 * Stronger than droidmedia_smoke.cpp (which bypassed the loader and
 * called raw hybris_dlopen): runs the actual library entry points
 * libcamera_host_vdi_impl.so uses, so a regression in the loader
 * surfaces here without flashing the VDI.
 *
 * Run on device (must be root for the binder NS bind to succeed):
 *     droidmedia_loader_smoke
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "droidmedia/droidmedia_loader.h"

int main()
{
    std::printf("droidmedia_loader_smoke — N12.D.3\n");

    auto &loader = OHOS::Camera::Hybris::Droid::Loader::Get();
    if (!loader.Ready()) {
        std::printf("FAIL loader not ready (see hilog CAMERA_VDI for "
                    "the exact reason — typically missing CAP_SYS_ADMIN "
                    "for unshare/bind)\n");
        return 1;
    }
    std::printf("OK   Droid::Loader ready\n");

    int n = loader.GetNumberOfCameras();
    std::printf("camera count = %d\n", n);
    if (n <= 0) {
        std::printf("FAIL no cameras enumerated\n");
        return 2;
    }
    for (int i = 0; i < n; ++i) {
        DroidMediaCameraInfo info{};
        if (!loader.GetInfo(&info, i)) {
            std::printf("  camera %d: get_info FAILED\n", i);
            continue;
        }
        std::printf("  camera %d: facing=%d orientation=%d°\n",
                    i, info.facing, info.orientation);
    }

    /*
     * Connect / immediately disconnect each camera to validate the
     * cam pointer + lifecycle.  Skips actual preview start.
     */
    for (int i = 0; i < n; ++i) {
        DroidMediaCamera *cam = loader.Connect(i);
        if (cam == nullptr) {
            std::printf("  camera %d: connect FAILED\n", i);
            continue;
        }
        std::printf("  camera %d: connect OK (cam=%p)\n", i, (void *)cam);
        if (char *p = loader.GetParameters(cam)) {
            size_t len = std::strlen(p);
            std::printf("    params (%zu B, first 120): %.*s%s\n",
                        len, (int)(len < 120 ? len : 120), p,
                        len < 120 ? "" : "…");
            /* upstream get_parameters returns strdup'd memory — free
             * to keep valgrind happy in case we call repeatedly. */
            std::free(p);
        }
        loader.Disconnect(cam);
        std::printf("  camera %d: disconnect OK\n", i);
    }

    std::printf("OK   loader smoke passed\n");
    return 0;
}
