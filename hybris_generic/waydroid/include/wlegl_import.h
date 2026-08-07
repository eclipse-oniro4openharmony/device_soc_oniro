/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * wlegl_import — Android gralloc handle → OHOS SurfaceBuffer.
 *
 * The one novel piece of the Waydroid display bridge (plan §W2): every
 * `android_wlegl` buffer a container client commits arrives as a full
 * `native_handle_t` (all fds + MTK-private ints — nothing less can be
 * re-imported, see plan §2.4). This wraps that handle into an OHOS
 * `BufferHandle` the same way the buffer VDI's AllocMem does, adopts it
 * into a `SurfaceBuffer`, and queues it with
 * `AttachAndFlushBuffer(needMap=false)` — no copies, no CPU locks.
 *
 * Must run in an init-started native process: appspawn'd apps cannot
 * reach the nested /android/vendor mount, so the gralloc mapper this
 * depends on would LOG_ALWAYS_FATAL there.
 */

#ifndef WAYDROID_WLEGL_IMPORT_H
#define WAYDROID_WLEGL_IMPORT_H

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <buffer_handle.h>
#include <surface.h>
#include <surface_buffer.h>

struct native_handle;
typedef const struct native_handle *buffer_handle_t;

namespace OHOS {
namespace Waydroid {

/* Geometry the client declared on the wire; the handle itself carries no
 * description of its own layout that we can trust across the boundary. */
struct WleglBufferDesc {
    int32_t width  = 0;
    int32_t height = 0;
    int32_t stride = 0;   /* PIXELS, as android_wlegl sends it */
    int32_t format = 0;   /* Android HAL_PIXEL_FORMAT_* */
    int64_t usage  = 0;   /* Android GRALLOC_USAGE_* */
};

/*
 * Owns the wl_buffer → SurfaceBuffer cache for one output Surface.
 *
 * Steady state is attach-once + flush-per-commit: Import() is called on
 * the first commit of each wl_buffer and returns the cached SurfaceBuffer
 * afterwards, so the per-frame cost is a single Flush IPC.
 */
class BufferImporter {
public:
    BufferImporter() = default;
    ~BufferImporter();

    BufferImporter(const BufferImporter&) = delete;
    BufferImporter& operator=(const BufferImporter&) = delete;

    /*
     * Wrap `handle` (owned by the caller — we dup every fd we keep) into a
     * SurfaceBuffer, caching it under `key` (the wl_buffer resource
     * pointer). Returns nullptr on failure.
     */
    sptr<SurfaceBuffer> Import(const void* key, buffer_handle_t handle,
                               const WleglBufferDesc& desc);

    /* Drop the cache entry for a destroyed wl_buffer. */
    void Forget(const void* key);

    void Clear();

    size_t CachedCount() const { return cache_.size(); }

private:
    std::unordered_map<const void*, sptr<SurfaceBuffer>> cache_;
};

/*
 * Build an OHOS BufferHandle from an Android native handle, mirroring
 * hybris_buffer_vdi_impl.cpp::AllocMem: fd = data[0], reserveFds =
 * numFds-1, reserveInts = numInts + kPtrSlots (trailing slots zeroed =
 * HANDLE_OWNER_NONE so FreeMem only closes fds), stride converted to
 * bytes, format mapped to the OHOS enum, size clamped to the largest fd.
 * Every fd is dup'd exactly once; caller passes ownership to
 * SurfaceBuffer::SetBufferHandle or must FreeBufferHandle it.
 */
BufferHandle* BuildBufferHandle(buffer_handle_t handle,
                                const WleglBufferDesc& desc);

/* Android HAL_PIXEL_FORMAT_* → OHOS PIXEL_FMT_*; 0 if unmapped. */
int32_t AndroidFormatToOhos(int32_t androidFormat);

} // namespace Waydroid
} // namespace OHOS

#endif // WAYDROID_WLEGL_IMPORT_H
