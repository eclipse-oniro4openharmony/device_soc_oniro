/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * N12.5.0 — libhybris HIDL smoke test.
 *
 * Smallest possible test that the libhybris bionic linker can load
 * Halium's HIDL shared libraries from an OHOS-side process: dlopen
 * libhidlbase + the camera-provider HIDL .so, dlsym a known C symbol
 * from each, and exit cleanly.  Success here is the precondition for
 * the N12.5.1+ bionic-side shim.
 *
 * Run on device:
 *     camera_hidl_smoke
 * Or via hilog with timeout wrap (see CLAUDE.md tips):
 *     timeout 5 camera_hidl_smoke 2>&1
 */

#include <cstdio>
#include <cstdlib>

#include "hybris/common/dlfcn.h"
#include "hybris_camera_log.h"

namespace {

constexpr const char *kLibHidlBase = "/android/system/lib64/libhidlbase.so";
constexpr const char *kLibHwBinder = "/android/system/lib64/libhwbinder.so";
constexpr const char *kLibUtils    = "/android/system/lib64/libutils.so";
constexpr const char *kLibProvider26 =
    "/android/system/lib64/android.hardware.camera.provider@2.6.so";

struct Probe {
    const char *path;
    const char *symbol;
};

bool TryLoad(const Probe &p)
{
    void *h = hybris_dlopen(p.path, /*RTLD_NOW=*/0x00002);
    if (h == nullptr) {
        const char *err = hybris_dlerror();
        std::printf("FAIL hybris_dlopen(%s): %s\n", p.path, err ? err : "(null)");
        HILOG_ERROR(LOG_CORE,
                    "smoke: hybris_dlopen FAIL %{public}s err=%{public}s",
                    p.path, err ? err : "(null)");
        return false;
    }
    std::printf("OK   hybris_dlopen(%s) = %p\n", p.path, h);
    HILOG_INFO(LOG_CORE,
               "smoke: hybris_dlopen OK %{public}s handle=%{public}p",
               p.path, h);

    if (p.symbol != nullptr) {
        void *s = hybris_dlsym(h, p.symbol);
        if (s == nullptr) {
            const char *err = hybris_dlerror();
            std::printf("WARN hybris_dlsym(%s, %s): %s\n",
                        p.path, p.symbol, err ? err : "(null)");
            HILOG_WARN(LOG_CORE,
                       "smoke: hybris_dlsym MISS %{public}s sym=%{public}s",
                       p.path, p.symbol);
        } else {
            std::printf("OK   hybris_dlsym(%s, %s) = %p\n",
                        p.path, p.symbol, s);
            HILOG_INFO(LOG_CORE,
                       "smoke: hybris_dlsym OK %{public}s sym=%{public}s addr=%{public}p",
                       p.path, p.symbol, s);
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    std::printf("camera_hidl_smoke — N12.5.0\n");
    HILOG_INFO(LOG_CORE, "smoke: camera_hidl_smoke starting");

    /*
     * Probe order matters: libutils and libhwbinder are bionic
     * underpinnings of libhidlbase; libhidlbase pulls them in
     * transitively, but loading them explicitly first surfaces any
     * dependency-chain failure with a clearer message.
     *
     * For the smoke test we look up benign C symbols that any HIDL
     * library defines: HIDL_FETCH_*, the C runtime entry points, or
     * the version vsdo symbol.  HIDL_FETCH_<InterfaceName> is the
     * canonical passthrough-loader entry point and is exported as
     * extern "C" — so it is dlsym-able without C++ mangling.
     */
    const Probe probes[] = {
        { kLibUtils,      nullptr },
        { kLibHwBinder,   nullptr },
        { kLibHidlBase,   nullptr },
        { kLibProvider26, "HIDL_FETCH_ICameraProvider" },
    };

    int failures = 0;
    for (const auto &p : probes) {
        if (!TryLoad(p)) failures++;
    }

    std::printf("\nsmoke: %d / %zu probes failed\n",
                failures, sizeof(probes) / sizeof(probes[0]));
    HILOG_INFO(LOG_CORE,
               "smoke: done failures=%{public}d total=%{public}zu",
               failures, sizeof(probes) / sizeof(probes[0]));

    return failures == 0 ? 0 : 1;
}
