/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * See wlegl_import.h for the design; the BufferHandle layout mirrored
 * here is documented in
 * device/soc/oniro/hybris_generic/hardware/display/include/
 * hybris_buffer_layout.h (kPtrSlots bookkeeping) and produced by that
 * component's AllocMem.
 */

#include "wlegl_import.h"

#include <unistd.h>
#include <sys/stat.h>

#include <cutils/native_handle.h>
#include <hilog/log.h>

#include "hybris_buffer_layout.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "waydroid_import"

namespace OHOS {
namespace Waydroid {

using OHOS::HDI::DISPLAY::kPtrSlots;

namespace {

/* Android HAL_PIXEL_FORMAT_* values we accept.  The container's
 * hwcomposer only ever commits RGB(A/X) for the desktop layer; the YUV
 * entries are here because video overlays reach the same path once
 * per-app windows land (W9). */
constexpr int32_t HAL_RGBA_8888 = 1;
constexpr int32_t HAL_RGBX_8888 = 2;
constexpr int32_t HAL_RGB_888   = 3;
constexpr int32_t HAL_RGB_565   = 4;
constexpr int32_t HAL_BGRA_8888 = 5;
constexpr int32_t HAL_YV12      = 0x32315659;
constexpr int32_t HAL_YCRCB_420_SP = 0x11;
constexpr int32_t HAL_YCBCR_420_888 = 0x23;

/* OHOS PIXEL_FMT_* (graphic_common_c.h) — spelled out so this file does
 * not drag the display HDI headers in. */
constexpr int32_t PIXEL_FMT_RGB_565     = 3;
constexpr int32_t PIXEL_FMT_RGBX_8888   = 8;
constexpr int32_t PIXEL_FMT_RGBA_8888   = 9;
constexpr int32_t PIXEL_FMT_BGRA_8888   = 12;
constexpr int32_t PIXEL_FMT_RGB_888     = 13;
constexpr int32_t PIXEL_FMT_YCBCR_420_SP = 34;
constexpr int32_t PIXEL_FMT_YCRCB_420_SP = 35;
constexpr int32_t PIXEL_FMT_YCRCB_420_P  = 37;

uint32_t BytesPerPixel(int32_t ohosFormat)
{
    switch (ohosFormat) {
        case PIXEL_FMT_RGBA_8888:
        case PIXEL_FMT_RGBX_8888:
        case PIXEL_FMT_BGRA_8888:
            return 4;
        case PIXEL_FMT_RGB_888:
            return 3;
        case PIXEL_FMT_RGB_565:
            return 2;
        default:
            return 1;   /* YUV planes: stride is already in bytes */
    }
}

/* The buffer size we advertise must never exceed what gralloc really
 * allocated — every CPU consumer sizes its access off BufferHandle::size
 * (the VDI learned this the hard way; see AllocMem's LargestFdBytes
 * comment). */
int64_t LargestFdBytes(const native_handle_t* nh)
{
    int64_t largest = 0;
    for (int i = 0; i < nh->numFds; ++i) {
        struct stat st {};
        if (fstat(nh->data[i], &st) == 0 && st.st_size > largest) {
            largest = static_cast<int64_t>(st.st_size);
        }
    }
    return largest;
}

} // namespace

int32_t AndroidFormatToOhos(int32_t androidFormat)
{
    switch (androidFormat) {
        case HAL_RGBA_8888:     return PIXEL_FMT_RGBA_8888;
        case HAL_RGBX_8888:     return PIXEL_FMT_RGBX_8888;
        case HAL_RGB_888:       return PIXEL_FMT_RGB_888;
        case HAL_RGB_565:       return PIXEL_FMT_RGB_565;
        case HAL_BGRA_8888:     return PIXEL_FMT_BGRA_8888;
        case HAL_YV12:          return PIXEL_FMT_YCRCB_420_P;
        case HAL_YCRCB_420_SP:  return PIXEL_FMT_YCRCB_420_SP;
        case HAL_YCBCR_420_888: return PIXEL_FMT_YCBCR_420_SP;
        default:                return 0;
    }
}

BufferHandle* BuildBufferHandle(buffer_handle_t handle,
                                const WleglBufferDesc& desc)
{
    const native_handle_t* nh = static_cast<const native_handle_t*>(handle);
    if (nh == nullptr || nh->numFds < 1) {
        HILOG_ERROR(LOG_CORE, "BuildBufferHandle: handle has no fds");
        return nullptr;
    }

    int32_t ohosFormat = AndroidFormatToOhos(desc.format);
    if (ohosFormat == 0) {
        HILOG_ERROR(LOG_CORE, "BuildBufferHandle: unmapped android format %{public}d",
                    desc.format);
        return nullptr;
    }

    uint32_t reserveFds  = static_cast<uint32_t>(nh->numFds - 1);
    uint32_t nativeInts  = static_cast<uint32_t>(nh->numInts);
    uint32_t reserveInts = nativeInts + kPtrSlots;

    size_t total = sizeof(BufferHandle) +
                   (reserveFds + reserveInts) * sizeof(int32_t);
    BufferHandle* bh = static_cast<BufferHandle*>(malloc(total));
    if (bh == nullptr) {
        HILOG_ERROR(LOG_CORE, "BuildBufferHandle: OOM (%{public}zu bytes)", total);
        return nullptr;
    }
    memset(bh, 0, total);

    uint32_t bpp = BytesPerPixel(ohosFormat);
    int32_t byteStride = desc.stride * static_cast<int32_t>(bpp);

    /* Every fd stored in the BufferHandle must be one we own: the handle
     * belongs to the wl_resource and outlives neither the client nor our
     * SurfaceBuffer reliably, and SurfaceBuffer's FreeBufferHandle path
     * closes what it is given.  A -1 anywhere would abort the parcel
     * read on the RS side, so bail out rather than store one. */
    bh->fd = dup(nh->data[0]);
    if (bh->fd < 0) {
        HILOG_ERROR(LOG_CORE, "BuildBufferHandle: dup(fd0) failed");
        free(bh);
        return nullptr;
    }
    for (uint32_t i = 0; i < reserveFds; ++i) {
        int fd = dup(nh->data[1 + i]);
        if (fd < 0) {
            HILOG_ERROR(LOG_CORE, "BuildBufferHandle: dup(fd%{public}u) failed", i + 1);
            close(bh->fd);
            for (uint32_t j = 0; j < i; ++j) {
                close(bh->reserve[j]);
            }
            free(bh);
            return nullptr;
        }
        bh->reserve[i] = fd;
    }
    for (uint32_t i = 0; i < nativeInts; ++i) {
        bh->reserve[reserveFds + i] = nh->data[nh->numFds + i];
    }
    /* Trailing kPtrSlots stay zero == HANDLE_OWNER_NONE: no process-local
     * pointer travels with this handle, so FreeMem just closes fds. */

    bh->width       = desc.width;
    bh->height      = desc.height;
    bh->stride      = byteStride;
    bh->format      = ohosFormat;
    bh->usage       = static_cast<uint64_t>(desc.usage);
    bh->virAddr     = nullptr;
    bh->phyAddr     = 0;
    bh->reserveFds  = reserveFds;
    bh->reserveInts = reserveInts;

    int64_t logical = static_cast<int64_t>(byteStride) * desc.height;
    if (ohosFormat == PIXEL_FMT_YCRCB_420_P ||
        ohosFormat == PIXEL_FMT_YCRCB_420_SP ||
        ohosFormat == PIXEL_FMT_YCBCR_420_SP) {
        logical = logical * 3 / 2;   /* + chroma */
    }
    int64_t allocated = LargestFdBytes(nh);
    bh->size = static_cast<int32_t>(
        (allocated > 0 && allocated < logical) ? allocated : logical);

    return bh;
}

BufferImporter::~BufferImporter()
{
    Clear();
}

sptr<SurfaceBuffer> BufferImporter::Import(const void* key,
                                           buffer_handle_t handle,
                                           const WleglBufferDesc& desc)
{
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }

    BufferHandle* bh = BuildBufferHandle(handle, desc);
    if (bh == nullptr) {
        return nullptr;
    }

    sptr<SurfaceBuffer> sb = SurfaceBuffer::Create();
    if (sb == nullptr) {
        HILOG_ERROR(LOG_CORE, "Import: SurfaceBuffer::Create failed");
        FreeBufferHandle(bh);
        return nullptr;
    }
    /* Takes ownership of bh; tolerates our VDI returning NOT_SUPPORT
     * from RegisterBuffer. */
    sb->SetBufferHandle(bh);

    BufferRequestConfig cfg = {
        .width  = desc.width,
        .height = desc.height,
        .strideAlignment = 1,
        .format = bh->format,
        .usage  = bh->usage,
        .timeout = 0,
    };
    sb->SetBufferRequestConfig(cfg);

    cache_.emplace(key, sb);
    HILOG_INFO(LOG_CORE,
               "Import: %{public}dx%{public}d fmt=%{public}d byteStride=%{public}d "
               "size=%{public}d fds=%{public}u cached=%{public}zu",
               desc.width, desc.height, bh->format, bh->stride, bh->size,
               bh->reserveFds + 1, cache_.size());
    return sb;
}

void BufferImporter::Forget(const void* key)
{
    cache_.erase(key);
}

void BufferImporter::Clear()
{
    cache_.clear();
}

} // namespace Waydroid
} // namespace OHOS
