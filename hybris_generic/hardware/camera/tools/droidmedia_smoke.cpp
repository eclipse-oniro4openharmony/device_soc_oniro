/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * N12.D.2 — droidmedia bring-up smoke test.
 *
 * Acceptance gate for the droidmedia pivot (phase_n12_camera_droidmedia.md).
 * hybris_dlopens /android/system/lib64/libdroidmedia.so from an OHOS-side
 * musl process, resolves the camera enumeration C ABI, calls init +
 * get_number_of_cameras + get_info for each camera.  Expected on the X23:
 *   - 3 cameras (S5KGM1ST rear primary, OV16A1Q front, GC08A3WIDE rear-wide)
 *   - sensible facing (0=front, 1=back) and orientation (multiple of 90°)
 *
 * Run on device:
 *     droidmedia_smoke
 * Or with timeout wrap (CLAUDE.md tip — hilog otherwise runs indefinitely):
 *     timeout 10 droidmedia_smoke 2>&1
 *
 * Note on the init symbol: upstream droidmedia.h declares
 *   bool droid_media_init();
 * but the libdroidmedia.so shipped on the Halium X23 rootfs exports the
 * older internal name
 *   void _droid_media_init();
 * Both are equivalent (both just kick the AOSP binder thread pool); we
 * resolve whichever is present and adapt the call shape.
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sched.h>
#include <sys/mount.h>

#include "droidmedia/droidmediacamera.h"
#include "hybris/common/dlfcn.h"
#include "hybris_camera_log.h"

namespace {

constexpr const char *kLibDroidMedia = "/android/system/lib64/libdroidmedia.so";

/*
 * Halium's AOSP cameraserver registers "media.camera" with the binder
 * driver provisioned by androidd as /dev/binderfs/android-binder (major
 * 510 minor 7).  Inside Halium's own mount NS that device is bound to
 * /dev/binder and AOSP libbinder hard-codes ProcessState::self() to
 * open("/dev/binder").  An OHOS-side process sees /dev/binder pointing
 * at OHOS's own binder (510:1) instead, so libdroidmedia.so dlopen
 * constructors (which run ProcessState::self() to start the binder
 * thread pool) end up bound to the wrong driver and the eventual
 * servicemanager call hangs.
 *
 * Fix: unshare a private mount NS and bind /dev/binderfs/android-binder
 * over /dev/binder before any libbinder constructor runs.  Same trick
 * androidd uses internally — see device/board/oniro/hybris_generic/
 * launcher/androidd.c:378.  Requires CAP_SYS_ADMIN (the smoke runs as
 * root from /vendor/bin so that's free here).  Idempotent: re-binding a
 * bind mount is a no-op except on the kernel mount tree.
 */
constexpr const char *kBinderHost    = "/dev/binderfs/android-binder";
constexpr const char *kBinderClient  = "/dev/binder";

bool BindAndroidBinder()
{
    if (unshare(CLONE_NEWNS) < 0) {
        std::printf("FAIL unshare(CLONE_NEWNS): %s\n", std::strerror(errno));
        HILOG_ERROR(LOG_CORE,
                    "droidmedia_smoke: unshare(CLONE_NEWNS) %{public}s",
                    std::strerror(errno));
        return false;
    }
    /*
     * Make / a slave-recursive so our bind mount stays scoped to this
     * process tree and doesn't propagate back into OHOS's mount table.
     * MS_PRIVATE would do the same; we use SLAVE so we still see
     * upstream mount events (e.g. /storage userdata bind that init may
     * add later) — harmless either way for a one-shot smoke.
     */
    if (mount(nullptr, "/", nullptr, MS_REC | MS_SLAVE, nullptr) < 0) {
        std::printf("FAIL mount(/ rslave): %s\n", std::strerror(errno));
        HILOG_ERROR(LOG_CORE, "droidmedia_smoke: rslave / %{public}s",
                    std::strerror(errno));
        return false;
    }
    if (mount(kBinderHost, kBinderClient, nullptr, MS_BIND, nullptr) < 0) {
        std::printf("FAIL bind %s -> %s: %s\n",
                    kBinderHost, kBinderClient, std::strerror(errno));
        HILOG_ERROR(LOG_CORE,
                    "droidmedia_smoke: bind %{public}s -> %{public}s %{public}s",
                    kBinderHost, kBinderClient, std::strerror(errno));
        return false;
    }
    std::printf("OK   bind %s over %s\n", kBinderHost, kBinderClient);
    HILOG_INFO(LOG_CORE,
               "droidmedia_smoke: bound %{public}s over %{public}s",
               kBinderHost, kBinderClient);
    return true;
}

/* RTLD_NOW from bionic <dlfcn.h> — libhybris's hybris_dlopen takes the
 * bionic flag value (0x2), not the host musl one. */
constexpr int kRtldNow = 0x00002;

using FnInit       = bool (*)();
using FnInitVoid   = void (*)();
using FnGetNumber  = int  (*)();
using FnGetInfo    = bool (*)(DroidMediaCameraInfo *, int);

bool RunInit(void *lib)
{
    /*
     * Try the modern public name first (returns bool).  Fall back to the
     * underscore-prefixed legacy name (returns void) used in older
     * droidmedia builds — both are wired to the same AOSP
     * ProcessState::startThreadPool() call underneath.
     */
    if (auto init = (FnInit)hybris_dlsym(lib, "droid_media_init")) {
        bool ok = init();
        std::printf("droid_media_init() returned %s\n", ok ? "true" : "false");
        HILOG_INFO(LOG_CORE, "droidmedia_smoke: droid_media_init -> %{public}d",
                   (int)ok);
        return ok;
    }
    if (auto init = (FnInitVoid)hybris_dlsym(lib, "_droid_media_init")) {
        init();
        std::printf("_droid_media_init() (legacy, void) called\n");
        HILOG_INFO(LOG_CORE, "droidmedia_smoke: _droid_media_init (legacy) called");
        return true;
    }
    std::printf("FAIL no init symbol found in libdroidmedia.so\n");
    HILOG_ERROR(LOG_CORE, "droidmedia_smoke: no init symbol found");
    return false;
}

const char *FacingStr(int facing)
{
    switch (facing) {
        case DROID_MEDIA_CAMERA_FACING_FRONT: return "FRONT";
        case DROID_MEDIA_CAMERA_FACING_BACK:  return "BACK";
        default:                              return "?";
    }
}

} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    std::printf("droidmedia_smoke — N12.D.2\n");
    HILOG_INFO(LOG_CORE, "droidmedia_smoke: starting");

    if (!BindAndroidBinder()) {
        return 6;
    }

    void *lib = hybris_dlopen(kLibDroidMedia, kRtldNow);
    if (lib == nullptr) {
        const char *err = hybris_dlerror();
        std::printf("FAIL hybris_dlopen(%s): %s\n",
                    kLibDroidMedia, err ? err : "(null)");
        HILOG_ERROR(LOG_CORE,
                    "droidmedia_smoke: hybris_dlopen FAIL %{public}s err=%{public}s",
                    kLibDroidMedia, err ? err : "(null)");
        return 1;
    }
    std::printf("OK   hybris_dlopen(%s) = %p\n", kLibDroidMedia, lib);
    HILOG_INFO(LOG_CORE, "droidmedia_smoke: dlopen OK lib=%{public}p", lib);

    if (!RunInit(lib)) {
        return 2;
    }

    auto get_n = (FnGetNumber)hybris_dlsym(lib,
        "droid_media_camera_get_number_of_cameras");
    auto get_info = (FnGetInfo)hybris_dlsym(lib,
        "droid_media_camera_get_info");
    if (get_n == nullptr || get_info == nullptr) {
        std::printf("FAIL dlsym get_number_of_cameras=%p get_info=%p\n",
                    (void *)get_n, (void *)get_info);
        HILOG_ERROR(LOG_CORE,
                    "droidmedia_smoke: dlsym FAIL get_n=%{public}p get_info=%{public}p",
                    (void *)get_n, (void *)get_info);
        return 3;
    }

    int n = get_n();
    std::printf("\ndroid_media_camera_get_number_of_cameras() = %d\n", n);
    HILOG_INFO(LOG_CORE, "droidmedia_smoke: %{public}d cameras", n);

    int failures = 0;
    for (int i = 0; i < n; ++i) {
        DroidMediaCameraInfo info{};
        if (!get_info(&info, i)) {
            std::printf("  camera %d: get_info FAILED\n", i);
            HILOG_ERROR(LOG_CORE,
                        "droidmedia_smoke: camera %{public}d get_info FAILED", i);
            failures++;
            continue;
        }
        std::printf("  camera %d: facing=%d (%s) orientation=%d°\n",
                    i, info.facing, FacingStr(info.facing), info.orientation);
        HILOG_INFO(LOG_CORE,
                   "droidmedia_smoke: cam %{public}d facing=%{public}d "
                   "orientation=%{public}d",
                   i, info.facing, info.orientation);
    }

    /*
     * Acceptance per N12.D.2: prints exactly 3 cameras with sensible
     * facing/orientation.  We treat n != 3 as a soft failure (returns
     * non-zero exit) but still report what we did get so any regression
     * is visible.
     */
    constexpr int kExpectedCameras = 3;
    if (n != kExpectedCameras) {
        std::printf("\nFAIL expected %d cameras, got %d\n",
                    kExpectedCameras, n);
        HILOG_ERROR(LOG_CORE,
                    "droidmedia_smoke: expected %{public}d cameras got %{public}d",
                    kExpectedCameras, n);
        return 4;
    }
    if (failures != 0) {
        std::printf("\nFAIL %d get_info failures\n", failures);
        return 5;
    }

    std::printf("\nOK   %d cameras enumerated successfully\n", n);
    HILOG_INFO(LOG_CORE, "droidmedia_smoke: done %{public}d cameras OK", n);
    return 0;
}
