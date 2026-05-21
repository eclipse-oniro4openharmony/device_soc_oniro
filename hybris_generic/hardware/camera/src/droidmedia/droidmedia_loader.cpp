/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "droidmedia/droidmedia_loader.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sched.h>
#include <sys/mount.h>

#include "hybris/common/dlfcn.h"
#include "hybris_camera_log.h"

namespace OHOS::Camera::Hybris::Droid {

namespace {

constexpr const char *kLibDroidMedia = "/android/system/lib64/libdroidmedia.so";
constexpr const char *kBinderHost    = "/dev/binderfs/android-binder";
constexpr const char *kBinderClient  = "/dev/binder";

/* RTLD_NOW from bionic <dlfcn.h> — libhybris's hybris_dlopen takes the
 * bionic flag value (0x2), not the host musl one (which is 0x1).  See
 * droidmedia_smoke.cpp for the original lookup. */
constexpr int kBionicRtldNow = 0x00002;

/*
 * libdroidmedia.so → libmedia.so → libandroidicu.so dependency chain
 * needs the Halium i18n apex on the bionic linker search path.  Init
 * sets this *before* the first hybris_dlopen so libhybris's lazy env
 * read sees it.  Keeps composer_host's /android/system/system/lib64
 * for parity (harmless if absent — bionic skips unreadable dirs). */
constexpr const char *kHybrisLdPath =
    "/android/vendor/lib64:"
    "/android/system/lib64:"
    "/android/system/system/lib64:"
    "/android/system/apex/com.android.i18n/lib64";

} // namespace

Loader &Loader::Get()
{
    static Loader instance;
    return instance;
}

Loader::Loader()
{
    if (!Init()) {
        CAMERA_VDI_LOGE("Droid::Loader: bring-up FAILED — "
                        "camera VDI will fall back to lcam001 placeholder");
    }
}

bool Loader::BindAndroidBinder()
{
    if (unshare(CLONE_NEWNS) < 0) {
        CAMERA_VDI_LOGE("Droid::Loader: unshare(CLONE_NEWNS) failed: %{public}s "
                        "— camera_host needs CAP_SYS_ADMIN in its .cfg",
                        std::strerror(errno));
        return false;
    }
    /*
     * MS_REC|MS_SLAVE: still see upstream mounts but our bind doesn't
     * propagate back to OHOS's tree.  MS_PRIVATE would do the same;
     * SLAVE is slightly friendlier if init.ohos later adds mounts we
     * care about (e.g. /storage user binds).  Harmless for a server
     * that won't see new mounts mid-life anyway.
     */
    if (mount(nullptr, "/", nullptr, MS_REC | MS_SLAVE, nullptr) < 0) {
        CAMERA_VDI_LOGE("Droid::Loader: mount(/, rslave) failed: %{public}s",
                        std::strerror(errno));
        return false;
    }
    if (mount(kBinderHost, kBinderClient, nullptr, MS_BIND, nullptr) < 0) {
        CAMERA_VDI_LOGE("Droid::Loader: bind %{public}s -> %{public}s failed: %{public}s",
                        kBinderHost, kBinderClient, std::strerror(errno));
        return false;
    }
    CAMERA_VDI_LOGI("Droid::Loader: bound %{public}s over %{public}s "
                    "(Halium AOSP binder visible as /dev/binder in this NS)",
                    kBinderHost, kBinderClient);
    return true;
}

void *Loader::Sym(const char *name)
{
    void *s = hybris_dlsym(lib_, name);
    if (s == nullptr) {
        const char *err = hybris_dlerror();
        CAMERA_VDI_LOGE("Droid::Loader: hybris_dlsym(%{public}s) FAIL %{public}s",
                        name, err ? err : "(null)");
    }
    return s;
}

template <typename F>
F Loader::LookupOrFail(const char *name)
{
    void *p = Sym(name);
    if (p == nullptr) {
        ready_ = false;
    }
    return reinterpret_cast<F>(p);
}

bool Loader::Init()
{
    if (!BindAndroidBinder()) {
        return false;
    }

    /*
     * Seed HYBRIS_LD_LIBRARY_PATH with the Halium apex paths
     * libdroidmedia.so transitively needs.  Don't override if the
     * service .cfg already set one — that way operators can debug
     * with a custom path via HYBRIS_LD_LIBRARY_PATH=... hdc shell run.
     */
    if (setenv("HYBRIS_LD_LIBRARY_PATH", kHybrisLdPath, /*overwrite=*/0) == 0) {
        const char *cur = getenv("HYBRIS_LD_LIBRARY_PATH");
        CAMERA_VDI_LOGI("Droid::Loader: HYBRIS_LD_LIBRARY_PATH=%{public}s",
                        cur ? cur : "(null)");
    }

    lib_ = hybris_dlopen(kLibDroidMedia, kBionicRtldNow);
    if (lib_ == nullptr) {
        const char *err = hybris_dlerror();
        CAMERA_VDI_LOGE("Droid::Loader: hybris_dlopen(%{public}s) FAIL %{public}s",
                        kLibDroidMedia, err ? err : "(null)");
        return false;
    }
    CAMERA_VDI_LOGI("Droid::Loader: hybris_dlopen %{public}s ok handle=%{public}p",
                    kLibDroidMedia, lib_);

    /*
     * Optimistic ready_ = true; LookupOrFail flips it back to false on
     * any missing mandatory symbol.  Init only finishes successfully if
     * the flag survives the lookups.
     */
    ready_ = true;

    /*
     * Init symbol: prefer modern droid_media_init (returns bool).  Fall
     * back to legacy _droid_media_init (void) — that's what the X23's
     * pre-built libdroidmedia.so exports.  At least one must exist.
     */
    init_        = reinterpret_cast<bool (*)()>(hybris_dlsym(lib_, "droid_media_init"));
    init_legacy_ = reinterpret_cast<void (*)()>(hybris_dlsym(lib_, "_droid_media_init"));
    if (init_ == nullptr && init_legacy_ == nullptr) {
        CAMERA_VDI_LOGE("Droid::Loader: no init symbol "
                        "(droid_media_init / _droid_media_init both missing)");
        ready_ = false;
        return false;
    }

    get_number_of_cameras_ =
        LookupOrFail<int (*)()>("droid_media_camera_get_number_of_cameras");
    get_info_ =
        LookupOrFail<bool (*)(DroidMediaCameraInfo *, int)>("droid_media_camera_get_info");

    connect_    = LookupOrFail<DroidMediaCamera *(*)(int)>("droid_media_camera_connect");
    disconnect_ = LookupOrFail<void (*)(DroidMediaCamera *)>("droid_media_camera_disconnect");

    start_preview_ = LookupOrFail<bool (*)(DroidMediaCamera *)>("droid_media_camera_start_preview");
    stop_preview_  = LookupOrFail<void (*)(DroidMediaCamera *)>("droid_media_camera_stop_preview");
    take_picture_  = LookupOrFail<bool (*)(DroidMediaCamera *, int)>("droid_media_camera_take_picture");

    set_parameters_ = LookupOrFail<bool (*)(DroidMediaCamera *, const char *)>(
        "droid_media_camera_set_parameters");
    get_parameters_ = LookupOrFail<char *(*)(DroidMediaCamera *)>(
        "droid_media_camera_get_parameters");

    set_callbacks_ = LookupOrFail<void (*)(DroidMediaCamera *,
                                            DroidMediaCameraCallbacks *, void *)>(
        "droid_media_camera_set_callbacks");

    get_buffer_queue_ = LookupOrFail<DroidMediaBufferQueue *(*)(DroidMediaCamera *)>(
        "droid_media_camera_get_buffer_queue");
    bq_set_callbacks_ = LookupOrFail<void (*)(DroidMediaBufferQueue *,
                                               DroidMediaBufferQueueCallbacks *, void *)>(
        "droid_media_buffer_queue_set_callbacks");

    buf_get_info_   = LookupOrFail<void (*)(DroidMediaBuffer *,
                                             DroidMediaBufferInfo *)>(
        "droid_media_buffer_get_info");
    buf_lock_ycbcr_ = LookupOrFail<bool (*)(DroidMediaBuffer *, uint32_t,
                                             DroidMediaBufferYCbCr *)>(
        "droid_media_buffer_lock_ycbcr");
    buf_unlock_  = LookupOrFail<void (*)(DroidMediaBuffer *)>("droid_media_buffer_unlock");
    buf_release_ = LookupOrFail<void (*)(DroidMediaBuffer *, void *, void *)>(
        "droid_media_buffer_release");
    buf_get_handle_ = LookupOrFail<const void *(*)(DroidMediaBuffer *)>(
        "droid_media_buffer_get_handle");

    if (!ready_) {
        CAMERA_VDI_LOGE("Droid::Loader: one or more mandatory symbols missing");
        return false;
    }

    if (init_) {
        bool ok = init_();
        if (!ok) {
            CAMERA_VDI_LOGE("Droid::Loader: droid_media_init() returned false");
            ready_ = false;
            return false;
        }
        CAMERA_VDI_LOGI("Droid::Loader: droid_media_init() OK");
    } else {
        init_legacy_();
        CAMERA_VDI_LOGI("Droid::Loader: _droid_media_init() (legacy, void) called");
    }

    return true;
}

/* ─── Passthrough wrappers ─────────────────────────────────────────── */

int Loader::GetNumberOfCameras()
{
    if (!ready_ || get_number_of_cameras_ == nullptr) return 0;
    return get_number_of_cameras_();
}

bool Loader::GetInfo(DroidMediaCameraInfo *info, int index)
{
    if (!ready_ || get_info_ == nullptr || info == nullptr) return false;
    return get_info_(info, index);
}

DroidMediaCamera *Loader::Connect(int index)
{
    if (!ready_ || connect_ == nullptr) return nullptr;
    return connect_(index);
}

void Loader::Disconnect(DroidMediaCamera *cam)
{
    if (!ready_ || disconnect_ == nullptr || cam == nullptr) return;
    disconnect_(cam);
}

bool Loader::StartPreview(DroidMediaCamera *cam)
{
    if (!ready_ || start_preview_ == nullptr || cam == nullptr) return false;
    return start_preview_(cam);
}

void Loader::StopPreview(DroidMediaCamera *cam)
{
    if (!ready_ || stop_preview_ == nullptr || cam == nullptr) return;
    stop_preview_(cam);
}

bool Loader::TakePicture(DroidMediaCamera *cam, int msgType)
{
    if (!ready_ || take_picture_ == nullptr || cam == nullptr) return false;
    return take_picture_(cam, msgType);
}

bool Loader::SetParameters(DroidMediaCamera *cam, const char *params)
{
    if (!ready_ || set_parameters_ == nullptr || cam == nullptr || params == nullptr)
        return false;
    return set_parameters_(cam, params);
}

char *Loader::GetParameters(DroidMediaCamera *cam)
{
    if (!ready_ || get_parameters_ == nullptr || cam == nullptr) return nullptr;
    return get_parameters_(cam);
}

void Loader::SetCallbacks(DroidMediaCamera *cam,
                           DroidMediaCameraCallbacks *cb, void *data)
{
    if (!ready_ || set_callbacks_ == nullptr || cam == nullptr) return;
    set_callbacks_(cam, cb, data);
}

DroidMediaBufferQueue *Loader::GetBufferQueue(DroidMediaCamera *cam)
{
    if (!ready_ || get_buffer_queue_ == nullptr || cam == nullptr) return nullptr;
    return get_buffer_queue_(cam);
}

void Loader::BufferQueueSetCallbacks(DroidMediaBufferQueue *bq,
                                      DroidMediaBufferQueueCallbacks *cb, void *data)
{
    if (!ready_ || bq_set_callbacks_ == nullptr || bq == nullptr) return;
    bq_set_callbacks_(bq, cb, data);
}

void Loader::BufferGetInfo(DroidMediaBuffer *buf, DroidMediaBufferInfo *info)
{
    if (!ready_ || buf_get_info_ == nullptr || buf == nullptr || info == nullptr) return;
    buf_get_info_(buf, info);
}

bool Loader::BufferLockYCbCr(DroidMediaBuffer *buf, uint32_t flags,
                              DroidMediaBufferYCbCr *out)
{
    if (!ready_ || buf_lock_ycbcr_ == nullptr || buf == nullptr || out == nullptr)
        return false;
    return buf_lock_ycbcr_(buf, flags, out);
}

void Loader::BufferUnlock(DroidMediaBuffer *buf)
{
    if (!ready_ || buf_unlock_ == nullptr || buf == nullptr) return;
    buf_unlock_(buf);
}

void Loader::BufferRelease(DroidMediaBuffer *buf, void *display, void *fence)
{
    if (!ready_ || buf_release_ == nullptr || buf == nullptr) return;
    buf_release_(buf, display, fence);
}

const void *Loader::BufferGetHandle(DroidMediaBuffer *buf)
{
    if (!ready_ || buf_get_handle_ == nullptr || buf == nullptr) return nullptr;
    return buf_get_handle_(buf);
}

} // namespace OHOS::Camera::Hybris::Droid
