/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Process-local loader for /android/system/lib64/libdroidmedia.so.
 *
 * Background — phase_n12_camera_droidmedia.md (N12.D).  We bind to
 * Halium's camera HAL via the pure-C libdroidmedia ABI (the same
 * library SailfishOS, Droidian and postmarketOS use).  Loading happens
 * exactly once per process (Meyers' singleton); subsequent VDI calls
 * are dispatched through the cached function-pointer table.
 *
 * Two preconditions before dlopen:
 *
 *   1. HYBRIS_LD_LIBRARY_PATH must include the Halium apex dirs (in
 *      particular /android/system/apex/com.android.i18n/lib64) — the
 *      libdroidmedia.so dep chain pulls in libandroidicu via libmedia.
 *      Wired in the camera_host startup .cfg (see N12.D production
 *      wiring task).
 *
 *   2. /dev/binder in our mount NS must point to Halium's
 *      android-binder driver (binderfs major 510 minor 7), not OHOS's
 *      default binder (510:1).  AOSP libbinder ProcessState::self() hard
 *      codes "/dev/binder" so we bind /dev/binderfs/android-binder over
 *      /dev/binder in a private mount NS before the dlopen.  Requires
 *      CAP_SYS_ADMIN — granted to the camera_host service via its cfg.
 *
 * Both setup steps live in `Init()`; on failure the loader is left in
 * an unusable state and every `Ready()` call returns false.
 */

#ifndef HYBRIS_DROIDMEDIA_LOADER_H
#define HYBRIS_DROIDMEDIA_LOADER_H

#include <cstdint>

#include "droidmedia/droidmediacamera.h"

namespace OHOS::Camera::Hybris::Droid {

class Loader {
public:
    /*
     * Process-wide singleton.  The first call attempts the full
     * dlopen+init sequence; subsequent calls are O(1).
     */
    static Loader &Get();

    /* True once Init has completed and all required symbols resolved. */
    bool Ready() const { return ready_; }

    /* ─── droidmedia C ABI passthrough ──────────────────────────────
     * Thin wrappers so VDI code reads as
     *   `Loader::Get().GetNumberOfCameras()`
     * instead of jumping through raw function pointers everywhere.
     * Every wrapper asserts Ready() in debug — in release it returns a
     * conservative no-op (false/0/nullptr) so a misload doesn't crash.
     */

    int  GetNumberOfCameras();
    bool GetInfo(DroidMediaCameraInfo *info, int index);

    DroidMediaCamera *Connect(int index);
    void              Disconnect(DroidMediaCamera *cam);

    bool StartPreview(DroidMediaCamera *cam);
    void StopPreview(DroidMediaCamera *cam);
    bool TakePicture(DroidMediaCamera *cam, int msgType);

    bool  SetParameters(DroidMediaCamera *cam, const char *params);
    char *GetParameters(DroidMediaCamera *cam);

    void SetCallbacks(DroidMediaCamera *cam,
                      DroidMediaCameraCallbacks *cb, void *data);

    DroidMediaBufferQueue *GetBufferQueue(DroidMediaCamera *cam);
    void BufferQueueSetCallbacks(DroidMediaBufferQueue *bq,
                                 DroidMediaBufferQueueCallbacks *cb, void *data);

    void BufferGetInfo(DroidMediaBuffer *buf, DroidMediaBufferInfo *info);
    bool BufferLockYCbCr(DroidMediaBuffer *buf, uint32_t flags,
                         DroidMediaBufferYCbCr *out);
    void BufferUnlock(DroidMediaBuffer *buf);
    void BufferRelease(DroidMediaBuffer *buf, void *display, void *fence);
    const void *BufferGetHandle(DroidMediaBuffer *buf);

private:
    Loader();
    ~Loader() = default;
    Loader(const Loader &) = delete;
    Loader &operator=(const Loader &) = delete;

    /*
     * One-shot bring-up.  Returns true on full success; false leaves
     * `ready_` false and every passthrough returns the no-op default.
     */
    bool Init();

    /* unshare(CLONE_NEWNS) + bind /dev/binderfs/android-binder over
     * /dev/binder.  See header note #2.  Caller is the same thread
     * that subsequently performs the dlopen. */
    static bool BindAndroidBinder();

    /* Resolve a libdroidmedia C symbol via hybris_dlsym.  Logs on
     * failure (with the symbol name) so a partial dep mismatch is
     * easy to diagnose. */
    void *Sym(const char *name);

    /* Mandatory-symbol wrapper around Sym: marks loader unusable if
     * the symbol is missing and stashes the first missing name for
     * the diag log. */
    template <typename F>
    F LookupOrFail(const char *name);

    void  *lib_   = nullptr;
    bool   ready_ = false;

    /* libdroidmedia function-pointer table.  Filled by Init(); zeroed
     * pointers are never called from passthroughs because every
     * passthrough early-returns on `!ready_`. */

    /* init: legacy void variant only on the on-device build (see
     * droidmedia_smoke.cpp note); modern droid_media_init exists on
     * upstream and is preferred when present. */
    bool (*init_)()       = nullptr;
    void (*init_legacy_)()= nullptr;

    int  (*get_number_of_cameras_)()                       = nullptr;
    bool (*get_info_)(DroidMediaCameraInfo *, int)          = nullptr;

    DroidMediaCamera *(*connect_)(int)                      = nullptr;
    void              (*disconnect_)(DroidMediaCamera *)    = nullptr;

    bool (*start_preview_)(DroidMediaCamera *)              = nullptr;
    void (*stop_preview_)(DroidMediaCamera *)               = nullptr;
    bool (*take_picture_)(DroidMediaCamera *, int)          = nullptr;

    bool  (*set_parameters_)(DroidMediaCamera *, const char *) = nullptr;
    char *(*get_parameters_)(DroidMediaCamera *)               = nullptr;

    void (*set_callbacks_)(DroidMediaCamera *,
                           DroidMediaCameraCallbacks *, void *) = nullptr;

    DroidMediaBufferQueue *(*get_buffer_queue_)(DroidMediaCamera *) = nullptr;
    void (*bq_set_callbacks_)(DroidMediaBufferQueue *,
                              DroidMediaBufferQueueCallbacks *, void *) = nullptr;

    void (*buf_get_info_)(DroidMediaBuffer *, DroidMediaBufferInfo *) = nullptr;
    bool (*buf_lock_ycbcr_)(DroidMediaBuffer *, uint32_t,
                            DroidMediaBufferYCbCr *)                  = nullptr;
    void (*buf_unlock_)(DroidMediaBuffer *)                           = nullptr;
    void (*buf_release_)(DroidMediaBuffer *, void *, void *)          = nullptr;
    const void *(*buf_get_handle_)(DroidMediaBuffer *)                = nullptr;
};

} // namespace OHOS::Camera::Hybris::Droid

#endif // HYBRIS_DROIDMEDIA_LOADER_H
