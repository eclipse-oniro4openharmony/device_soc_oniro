/*
 * Copyright (c) 2024 Oniro Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hybris_buffer_vdi_impl.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>

#include "display_common.h"
#include "hdf_base.h"
#include "hybris_buffer_layout.h"
#include "parameter.h"

/* libhybris gralloc API */
#include <hybris/gralloc/gralloc.h>
/* Android native_handle_t */
#include <cutils/native_handle.h>
/* Android HAL pixel format values */
#include <system/graphics-base-v1.0.h>
/* Android GRALLOC_USAGE_* values */
#include <hardware/gralloc.h>

/* Standard dlfcn for RTLD_LAZY / RTLD_GLOBAL — musl's version, not hybris's */
#include <dlfcn.h>

/* android_dlopen from libhybris-common — forward declaration matches binding.h */
extern "C" void* android_dlopen(const char* filename, int flags);

namespace OHOS {
namespace HDI {
namespace DISPLAY {

using namespace OHOS::HDI::Display::Buffer::V1_0;

/* ─── Format mapping ──────────────────────────────────────────────────────── */

/*
 * OHOS PixelFormat (display_type.h enum, sequential from 0) →
 * Android HAL_PIXEL_FORMAT_* (system/graphics-base-v1.0.h).
 *
 * OHOS enum values (not defined in a header we can use, so listed explicitly):
 *   PIXEL_FMT_CLUT8=0, CLUT1=1, CLUT4=2, RGB_565=3, RGBA_5658=4,
 *   RGBX_4444=5, RGBA_4444=6, RGB_444=7, RGBX_5551=8, RGBA_5551=9,
 *   RGB_555=10, RGBX_8888=11, RGBA_8888=12, RGB_888=13, BGR_565=14,
 *   BGRX_4444=15, BGRA_4444=16, BGRX_5551=17, BGRA_5551=18, BGRX_8888=19,
 *   BGRA_8888=20, YUV_422_I=21, YCBCR_422_SP=22, YCRCB_422_SP=23,
 *   YCBCR_420_SP=24, YCRCB_420_SP=25, YCBCR_422_P=26, YCRCB_422_P=27,
 *   YCBCR_420_P=28, YCRCB_420_P=29, YUYV_422_PKG=30, UYVY_422_PKG=31,
 *   YVYU_422_PKG=32, VYUY_422_PKG=33.
 */
static int OhosFormatToAndroid(uint32_t ohosFormat)
{
    switch (ohosFormat) {
        case 3:  return HAL_PIXEL_FORMAT_RGB_565;        /* PIXEL_FMT_RGB_565  */
        case 11: return HAL_PIXEL_FORMAT_RGBX_8888;      /* PIXEL_FMT_RGBX_8888 */
        case 12: return HAL_PIXEL_FORMAT_RGBA_8888;      /* PIXEL_FMT_RGBA_8888 */
        case 13: return HAL_PIXEL_FORMAT_RGB_888;        /* PIXEL_FMT_RGB_888  */
        case 20: return HAL_PIXEL_FORMAT_BGRA_8888;      /* PIXEL_FMT_BGRA_8888 */
        case 22: return HAL_PIXEL_FORMAT_YCBCR_422_SP;   /* PIXEL_FMT_YCBCR_422_SP */
        case 23: return HAL_PIXEL_FORMAT_YCRCB_420_SP;   /* PIXEL_FMT_YCRCB_422_SP → closest */
        case 24: return HAL_PIXEL_FORMAT_YCBCR_420_888;  /* PIXEL_FMT_YCBCR_420_SP */
        case 25: return HAL_PIXEL_FORMAT_YCRCB_420_SP;   /* PIXEL_FMT_YCRCB_420_SP */
        case 21: return HAL_PIXEL_FORMAT_YCBCR_422_I;    /* PIXEL_FMT_YUV_422_I */
        case 38: return HAL_PIXEL_FORMAT_BLOB;           /* PIXEL_FMT_BLOB (v1_1): 1-D byte buffer
                                                          * (e.g. ASTC payloads from ImageSource);
                                                          * IMPLEMENTATION_DEFINED would make MTK
                                                          * gralloc pick a non-CPU-mappable layout */
        case 39: return HAL_PIXEL_FORMAT_RGBA_FP16;      /* PIXEL_FMT_RGBA16_FLOAT (v1_2) */
        default:
            DISPLAY_LOGW("Unknown OHOS format %{public}u, using IMPLEMENTATION_DEFINED", ohosFormat);
            return HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
    }
}

/* ─── Usage mapping ───────────────────────────────────────────────────────── */

/*
 * OHOS HBM_USE_* flags → Android GRALLOC_USAGE_*.
 * Source: DisplayComposerType.idl enum HBM_USE.
 */
static int OhosUsageToAndroid(uint64_t ohosUsage)
{
    int androidUsage = 0;

    if (ohosUsage & (1ULL << 0))  androidUsage |= GRALLOC_USAGE_SW_READ_RARELY;   /* HBM_USE_CPU_READ */
    if (ohosUsage & (1ULL << 1))  androidUsage |= GRALLOC_USAGE_SW_WRITE_RARELY;  /* HBM_USE_CPU_WRITE */
    if (ohosUsage & (1ULL << 6))  androidUsage |= GRALLOC_USAGE_HW_FB;            /* HBM_USE_MEM_FB */
    if (ohosUsage & (1ULL << 8))  androidUsage |= GRALLOC_USAGE_HW_RENDER;        /* HBM_USE_HW_RENDER */
    if (ohosUsage & (1ULL << 9))  androidUsage |= GRALLOC_USAGE_HW_TEXTURE;       /* HBM_USE_HW_TEXTURE */
    if (ohosUsage & (1ULL << 10)) androidUsage |= GRALLOC_USAGE_HW_COMPOSER;      /* HBM_USE_HW_COMPOSER */
    if (ohosUsage & (1ULL << 11)) androidUsage |= GRALLOC_USAGE_PROTECTED;        /* HBM_USE_PROTECTED */
    if (ohosUsage & (1ULL << 12)) androidUsage |= GRALLOC_USAGE_HW_CAMERA_READ;   /* HBM_USE_CAMERA_READ */
    if (ohosUsage & (1ULL << 13)) androidUsage |= GRALLOC_USAGE_HW_CAMERA_WRITE;  /* HBM_USE_CAMERA_WRITE */
    if (ohosUsage & (1ULL << 14)) androidUsage |= GRALLOC_USAGE_HW_VIDEO_ENCODER; /* HBM_USE_VIDEO_ENCODER */
    if (ohosUsage & (1ULL << 16)) androidUsage |= GRALLOC_USAGE_SW_READ_OFTEN;    /* HBM_USE_CPU_READ_OFTEN */

    /* Ensure GPU-renderable buffers have HW access and SW_READ/WRITE so the
     * OHOS buffer queue can CPU-map them via hybris_gralloc_lock in Mmap. */
    if (androidUsage & (GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER)) {
        androidUsage |= GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE
                     | GRALLOC_USAGE_SW_READ_RARELY | GRALLOC_USAGE_SW_WRITE_RARELY;
    }

    /* Default: if nothing is set, provide a sane baseline */
    if (androidUsage == 0) {
        androidUsage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_SW_READ_RARELY;
    }

    return androidUsage;
}

/* ─── native_handle_t pointer storage in BufferHandle::reserve[] ─────────── */

/* Layout of the trailing kPtrSlots bookkeeping slots: hybris_buffer_layout.h */

static void StoreNativeHandle(BufferHandle* bh, buffer_handle_t native, int32_t owner)
{
    uint32_t offset = bh->reserveFds + bh->reserveInts - kPtrSlots;
    uintptr_t p = reinterpret_cast<uintptr_t>(native);
    bh->reserve[offset]     = static_cast<int32_t>(p & 0xFFFFFFFFu);
    bh->reserve[offset + 1] = static_cast<int32_t>(p >> 32u);
    bh->reserve[offset + 2] = native ? static_cast<int32_t>(getpid()) : 0;
    bh->reserve[offset + 3] = native ? owner : HANDLE_OWNER_NONE;
}

static buffer_handle_t LoadNativeHandle(const BufferHandle& bh, int32_t* owner = nullptr)
{
    if (owner != nullptr) {
        *owner = HANDLE_OWNER_NONE;
    }
    if (bh.reserveFds + bh.reserveInts < kPtrSlots) {
        return nullptr;
    }
    uint32_t offset = bh.reserveFds + bh.reserveInts - kPtrSlots;
    if (bh.reserve[offset + 2] != static_cast<int32_t>(getpid())) {
        return nullptr;
    }
    uintptr_t lo = static_cast<uint32_t>(bh.reserve[offset]);
    uintptr_t hi = static_cast<uint32_t>(bh.reserve[offset + 1]);
    if (owner != nullptr) {
        *owner = bh.reserve[offset + 3];
    }
    return reinterpret_cast<buffer_handle_t>(lo | (hi << 32u));
}

/* ─── Gralloc mapper pre-load (same fix as test_hwcomposer) ──────────────── */

/*
 * GraphicBufferMapper::getInstance() loads the gralloc mapper via
 * android_load_sphal_library when the first GPU buffer operation is requested.
 * Pre-loading here ensures it is already in the hybris linker's table so
 * the SPHAL namespace bypass hook (_hybris_hook_android_load_sphal_library)
 * can intercept the call successfully.
 *
 * Returns whether a mapper implementation could be loaded.  In appspawn'd
 * processes the sandbox carries /android/system but not the nested
 * /android/vendor mount, so this fails there — and every libhybris gralloc
 * entry point would then LOG_ALWAYS_FATAL inside the GraphicBufferMapper
 * constructor instead of returning an error.  Callers must treat false as
 * "never touch hybris gralloc in this process" (Mmap falls back to direct
 * DMA-BUF fd mapping).
 */
static bool PreloadGrallocMapper()
{
    static const char* kMapperPaths[] = {
        "/android/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so",
        "/android/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl.so",
        nullptr,
    };

    for (int i = 0; kMapperPaths[i]; i++) {
        void* h = android_dlopen(kMapperPaths[i], RTLD_LAZY | RTLD_GLOBAL);
        if (h) {
            DISPLAY_LOGI("Pre-loaded gralloc mapper: %{public}s", kMapperPaths[i]);
            return true;
        }
    }
    DISPLAY_LOGW("Could not pre-load gralloc mapper; using direct DMA-BUF fd mapping");
    return false;
}

/* True once a gralloc mapper implementation is loaded in this process. */
static bool g_grallocUsable = false;

/* Debug override: force the DMA-BUF fd path even where gralloc works, so the
 * app-sandbox fallback can be exercised from a plain shell process. */
static bool ForceFdMmap()
{
    static bool force = getenv("HYBRIS_DISP_FORCE_FD_MMAP") != nullptr;
    return force;
}

static bool UseGralloc()
{
    return g_grallocUsable && !ForceFdMmap();
}

/*
 * Find the fd in a BufferHandle that actually backs the pixels.
 *
 * MTK gralloc handles carry several fds and the FIRST one (which AllocMem
 * mirrors into BufferHandle::fd) is an anon_inode:gralloc_extra metadata fd
 * that cannot be mmap'd at all; the real DMA-BUF is among the reserve fds.
 * The buffer fd is identified as the first one whose fstat size covers the
 * buffer (metadata fds report 0 or a few KiB).
 */
static int FindMappableFd(const BufferHandle& handle)
{
    int candidates[9];
    int count = 0;
    candidates[count++] = handle.fd;
    for (uint32_t i = 0; i < handle.reserveFds && count < 9; i++) {
        candidates[count++] = handle.reserve[i];
    }
    for (int i = 0; i < count; i++) {
        if (candidates[i] < 0) {
            continue;
        }
        struct stat st = {};
        if (fstat(candidates[i], &st) == 0 && st.st_size >= handle.size) {
            return candidates[i];
        }
    }
    return handle.fd;
}

/* ─── DMA-BUF cache maintenance for fd-mapped buffers ────────────────────── */

/*
 * When a buffer is CPU-mapped via plain mmap of its DMA-BUF fd (no gralloc
 * lock), cache coherency is our job: DMA_BUF_IOCTL_SYNC START invalidates
 * before CPU access, END flushes after.  Failure is non-fatal — coherent
 * allocations don't need it and the ioctl just returns an error.
 */
static void DmaBufSync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = {};
    sync.flags = flags;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        DISPLAY_LOGD("DMA_BUF_IOCTL_SYNC(0x%{public}llx) failed: %{public}s",
                     static_cast<unsigned long long>(flags), strerror(errno));
    }
}

/* ─── Constructor ────────────────────────────────────────────────────────── */

HybrisBufferVdiImpl::HybrisBufferVdiImpl()
{
    /*
     * Same readiness gate as the composer VDI (see InitHwc2Device): the
     * ctor itself only dlopens, but the first AllocMem reaches
     * GraphicBufferAllocator::get(), which LOG_ALWAYS_FATALs if the
     * Android allocator service isn't registered yet.  androidd flips
     * android.composer.ready when the container HAL fleet (composer +
     * allocator register together) is up.  Timeout → proceed, preserving
     * pre-gate behavior.
     */
    if (WaitParameter("android.composer.ready", "1", 90) != 0) {
        DISPLAY_LOGW("timed out waiting for android.composer.ready=1 — proceeding anyway");
    }

    g_grallocUsable = PreloadGrallocMapper();
    if (!g_grallocUsable) {
        /* App sandbox (no /android/vendor): hybris gralloc would abort on
         * first use, so leave it uninitialized — Mmap uses fd mapping. */
        return;
    }

    /*
     * Initialize libhybris gralloc without a framebuffer device.
     * On Android 10+ (GRALLOC_COMPAT path) this calls hybris_ui_initialize()
     * which loads GraphicBufferAllocator / Mapper via HIDL.
     */
    hybris_gralloc_initialize(0 /* no framebuffer */);
    DISPLAY_LOGI("HybrisBufferVdiImpl: gralloc initialized");
}

/* ─── AllocMem ───────────────────────────────────────────────────────────── */

int32_t HybrisBufferVdiImpl::AllocMem(const AllocInfo& info, BufferHandle*& handle) const
{
    if (!g_grallocUsable) {
        /* hybris_gralloc_allocate would abort in the GraphicBufferMapper
         * ctor; allocation belongs in allocator_host anyway. */
        DISPLAY_LOGE("AllocMem: no gralloc mapper in this process");
        return HDF_FAILURE;
    }

    int androidFormat = OhosFormatToAndroid(info.format);
    int androidUsage  = OhosUsageToAndroid(info.usage);

    buffer_handle_t nativeHandle = nullptr;
    uint32_t stride = 0;

    int ret = hybris_gralloc_allocate(
        static_cast<int>(info.width),
        static_cast<int>(info.height),
        androidFormat,
        androidUsage,
        &nativeHandle,
        &stride);

    if (ret != 0 || !nativeHandle) {
        DISPLAY_LOGE("hybris_gralloc_allocate failed: ret=%{public}d w=%{public}u h=%{public}u "
                     "fmt=%{public}d usage=0x%{public}x",
                     ret, info.width, info.height, androidFormat, androidUsage);
        return HDF_FAILURE;
    }

    const native_handle_t* nh = static_cast<const native_handle_t*>(nativeHandle);

    /*
     * Layout of BufferHandle::reserve[]:
     *   [0 .. reserveFds-1]                      — extra fds (nh->data[1..numFds-1])
     *   [reserveFds .. reserveFds+numInts-1]      — native ints (nh->data[numFds..])
     *   [reserveFds+numInts .. +kPtrSlots-1]      — handle pointer + owner tag
     */
    uint32_t reserveFds  = (nh->numFds > 0) ? static_cast<uint32_t>(nh->numFds - 1) : 0;
    uint32_t nativeInts  = static_cast<uint32_t>(nh->numInts);
    uint32_t reserveInts = nativeInts + kPtrSlots;

    size_t totalSize = sizeof(BufferHandle) + (reserveFds + reserveInts) * sizeof(int32_t);
    BufferHandle* bh  = static_cast<BufferHandle*>(malloc(totalSize));
    if (!bh) {
        DISPLAY_LOGE("AllocMem: OOM allocating BufferHandle (size=%{public}zu)", totalSize);
        hybris_gralloc_release(nativeHandle, 1);
        return HDF_ERR_MALLOC_FAIL;
    }
    memset(bh, 0, totalSize);

    /*
     * Convert Android pixel stride → OHOS byte stride.
     *
     * OHOS consumers (e.g. wmserver/surface_draw.cpp DoDrawCustomStartingWindow
     * and DoDrawImageRect) compute their bitmap width as
     *   alignWidth = buffer->GetStride() / IMAGE_BYTES_STRIDE  (=4)
     * which assumes stride is in BYTES.  The reference OHOS gralloc at
     * drivers/peripheral/display/hal/default_standard/src/display_gralloc/
     * allocator.cpp:166 also stores bytes.  Storing the raw Android pixel
     * stride here caused the launcher starting window to render its bitmap
     * into ~25% of the surface with the remainder left transparent.
     */
    uint32_t bpp = HybrisBytesPerPixelOhos(info.format);
    uint32_t byteStride = (bpp > 0) ? (stride * bpp) : stride;

    bh->fd          = (nh->numFds > 0) ? nh->data[0] : -1;
    bh->width       = static_cast<int32_t>(info.width);
    bh->height      = static_cast<int32_t>(info.height);
    bh->stride      = static_cast<int32_t>(byteStride);
    bh->format      = static_cast<int32_t>(info.format); /* keep OHOS format for upper layers */
    bh->usage       = info.usage;
    bh->size        = static_cast<int32_t>(byteStride * info.height); /* total bytes */
    bh->virAddr     = nullptr;
    bh->phyAddr     = 0;
    bh->reserveFds  = reserveFds;
    bh->reserveInts = reserveInts;

    /* Copy remaining fds */
    for (uint32_t i = 0; i < reserveFds; i++) {
        bh->reserve[i] = nh->data[1 + i];
    }
    /* Copy native ints */
    for (uint32_t i = 0; i < nativeInts; i++) {
        bh->reserve[reserveFds + i] = nh->data[nh->numFds + i];
    }
    /* Store the native handle pointer in the last kPtrSlots slots */
    StoreNativeHandle(bh, nativeHandle, HANDLE_OWNER_ALLOCATED);

    DISPLAY_LOGI("AllocMem: %{public}ux%{public}u fmt=%{public}d "
                 "pixStride=%{public}u byteStride=%{public}u size=%{public}d fd=%{public}d",
                 info.width, info.height, info.format, stride, byteStride, bh->size, bh->fd);

    handle = bh;
    return HDF_SUCCESS;
}

/* ─── FreeMem ────────────────────────────────────────────────────────────── */

void HybrisBufferVdiImpl::FreeMem(const BufferHandle& handle) const
{
    int32_t owner = HANDLE_OWNER_NONE;
    buffer_handle_t nativeHandle = LoadNativeHandle(handle, &owner);
    BufferHandle* bh = const_cast<BufferHandle*>(&handle);

    if (owner == HANDLE_OWNER_ALLOCATED) {
        /*
         * Releasing the allocation also closes its fds — which are the very
         * fds mirrored into this BufferHandle — so they must not be closed
         * again here or we would close an unrelated, since-reused fd.
         */
        hybris_gralloc_release(nativeHandle, 1 /* was_allocated */);
        free(bh);
        return;
    }

    if (owner == HANDLE_OWNER_IMPORTED) {
        /* Drops our import reference along with the fds importBuffer dup'd. */
        hybris_gralloc_release(nativeHandle, 0 /* just an import reference */);
    }

    /*
     * The buffer was allocated in another process (AllocMem runs in
     * allocator_host), which frees the allocation itself.  The fds this
     * BufferHandle carries were dup'd into us by the IPC layer, and
     * MapperService::FreeMem handed us ownership of it via
     * NativeBuffer::Move(), so closing them is our job — nobody else will.
     */
    if (bh->fd >= 0) {
        close(bh->fd);
        bh->fd = -1;
    }
    for (uint32_t i = 0; i < bh->reserveFds; i++) {
        if (bh->reserve[i] >= 0) {
            close(bh->reserve[i]);
            bh->reserve[i] = -1;
        }
    }
    free(bh);
}

/* ─── Mmap ───────────────────────────────────────────────────────────────── */

/*
 * Build a native_handle_t from the fds and ints serialised in a BufferHandle.
 *
 * AllocMem stores a raw process-local buffer_handle_t pointer in the trailing
 * reserve[] slots.  That pointer is meaningless once the BufferHandle has been
 * marshalled across an IPC boundary (the fds are dup'd, the ints are copied
 * verbatim, but the pointer addresses are from the allocating process).
 *
 * This helper reconstructs a valid native_handle_t from the portable fd/int
 * data so it can be imported with hybris_gralloc_import_buffer.
 */
static native_handle_t* ReconstructNativeHandle(const BufferHandle& handle)
{
    int numFds  = (handle.fd >= 0 ? 1 : 0) + static_cast<int>(handle.reserveFds);
    int numInts = static_cast<int>(handle.reserveInts) > static_cast<int>(kPtrSlots)
                      ? static_cast<int>(handle.reserveInts) - static_cast<int>(kPtrSlots)
                      : 0;

    native_handle_t* nh = native_handle_create(numFds, numInts);
    if (!nh) {
        return nullptr;
    }

    int fdIdx = 0;
    if (handle.fd >= 0) {
        nh->data[fdIdx++] = handle.fd;
    }
    for (int i = 0; i < static_cast<int>(handle.reserveFds); i++) {
        nh->data[fdIdx++] = handle.reserve[i];
    }
    for (int i = 0; i < numInts; i++) {
        nh->data[numFds + i] = handle.reserve[handle.reserveFds + i];
    }
    return nh;
}

void* HybrisBufferVdiImpl::Mmap(const BufferHandle& handle) const
{
    static const int kLockUsage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;

    /* Paths 1–2 require the Android mapper HAL, which appspawn'd processes
     * cannot load (no /android/vendor in the sandbox) — any hybris gralloc
     * call there would LOG_ALWAYS_FATAL in the GraphicBufferMapper ctor, so
     * such processes use only the DMA-BUF fd path below. */
    if (UseGralloc()) {
        /* Path 1: same-process — LoadNativeHandle only returns the stored
         * pointer when this process is the one that produced it. */
        buffer_handle_t nativeHandle = LoadNativeHandle(handle);
        if (nativeHandle) {
            void* vaddr = nullptr;
            int ret = hybris_gralloc_lock(nativeHandle, kLockUsage, 0, 0,
                                          handle.width, handle.height, &vaddr);
            if (ret == 0 && vaddr) {
                const_cast<BufferHandle&>(handle).virAddr = vaddr;
                return vaddr;
            }
            /* Fall through: cross-process stale pointer. */
        }

        /* Path 2: cross-process import — reconstruct handle from portable
         * fd/int data, import via gralloc, then lock. */
        native_handle_t* rawNh = ReconstructNativeHandle(handle);
        if (rawNh) {
            buffer_handle_t importedHandle = nullptr;
            int ret = hybris_gralloc_import_buffer(rawNh, &importedHandle);
            native_handle_delete(rawNh);
            if (ret == 0 && importedHandle) {
                void* vaddr = nullptr;
                ret = hybris_gralloc_lock(importedHandle, kLockUsage, 0, 0,
                                          handle.width, handle.height, &vaddr);
                if (ret == 0 && vaddr) {
                    StoreNativeHandle(const_cast<BufferHandle*>(&handle), importedHandle,
                                      HANDLE_OWNER_IMPORTED);
                    const_cast<BufferHandle&>(handle).virAddr = vaddr;
                    return vaddr;
                }
                hybris_gralloc_release(importedHandle, 0);
            }
            DISPLAY_LOGW("Mmap: gralloc import/lock failed, using fd mmap fallback");
        }
    }

    /* Path 3: direct DMA-BUF fd mmap — bypasses gralloc HAL entirely.
     * handle.fd itself may be a metadata fd (MTK gralloc_extra); pick the fd
     * that actually backs the pixels. */
    int bufFd = FindMappableFd(handle);
    if (bufFd >= 0 && handle.size > 0) {
        void* mmapAddr = ::mmap(nullptr, static_cast<size_t>(handle.size),
                                PROT_READ | PROT_WRITE, MAP_SHARED, bufFd, 0);
        if (mmapAddr == MAP_FAILED && (errno == EACCES || errno == EPERM)) {
            /* fds that crossed the HDI IPC boundary may arrive without write
             * mode; consumers in mapper-less processes only read (snapshot /
             * thumbnail decode), so a read-only mapping is still useful. */
            mmapAddr = ::mmap(nullptr, static_cast<size_t>(handle.size),
                              PROT_READ, MAP_SHARED, bufFd, 0);
            if (mmapAddr != MAP_FAILED) {
                DISPLAY_LOGW("Mmap: fd mmap is read-only (rw refused: EACCES)");
            }
        }
        if (mmapAddr != MAP_FAILED) {
            DmaBufSync(bufFd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
            DISPLAY_LOGI("Mmap: fd mmap fd=%{public}d size=%{public}d addr=%{public}p",
                         bufFd, handle.size, mmapAddr);
            const_cast<BufferHandle&>(handle).virAddr = mmapAddr;
            return mmapAddr;
        }
        DISPLAY_LOGE("Mmap: fd mmap failed: %{public}s (fd=%{public}d reqSize=%{public}d)",
                     strerror(errno), bufFd, handle.size);
    }
    DISPLAY_LOGE("Mmap: all paths failed %{public}dx%{public}d fd=%{public}d",
                 handle.width, handle.height, handle.fd);
    return nullptr;
}

/* ─── Unmap ──────────────────────────────────────────────────────────────── */

int32_t HybrisBufferVdiImpl::Unmap(const BufferHandle& handle) const
{
    int32_t owner = HANDLE_OWNER_NONE;
    buffer_handle_t nativeHandle = UseGralloc() ? LoadNativeHandle(handle, &owner) : nullptr;
    if (!nativeHandle) {
        /* Buffer was mapped via direct fd mmap — no gralloc unlock needed. */
        if (handle.virAddr && handle.size > 0) {
            int bufFd = FindMappableFd(handle);
            if (bufFd >= 0) {
                DmaBufSync(bufFd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
            }
            ::munmap(handle.virAddr, static_cast<size_t>(handle.size));
            const_cast<BufferHandle&>(handle).virAddr = nullptr;
            return HDF_SUCCESS;
        }
        DISPLAY_LOGE("Unmap: null native handle");
        return HDF_FAILURE;
    }

    int ret = hybris_gralloc_unlock(nativeHandle);
    if (ret != 0) {
        DISPLAY_LOGW("Unmap: hybris_gralloc_unlock returned %{public}d", ret);
    }

    /*
     * If this handle was imported cross-process in Mmap, release the import
     * reference now and clear the stored pointer so a subsequent Mmap
     * re-imports.  A handle we allocated ourselves keeps its pointer — only
     * FreeMem may release that one.
     */
    if (owner == HANDLE_OWNER_IMPORTED) {
        hybris_gralloc_release(nativeHandle, 0 /* just an import reference */);
        static constexpr buffer_handle_t kNullHandle = nullptr;
        StoreNativeHandle(const_cast<BufferHandle*>(&handle), kNullHandle, HANDLE_OWNER_NONE);
    }
    const_cast<BufferHandle&>(handle).virAddr = nullptr;
    return HDF_SUCCESS;
}

/* ─── FlushCache ─────────────────────────────────────────────────────────── */

int32_t HybrisBufferVdiImpl::FlushCache(const BufferHandle& handle) const
{
    /*
     * gralloc unlock implies a cache flush on all Android implementations.
     * We unlock and immediately re-lock to keep the mapping valid.
     * This matches the semantics expected by the OHOS buffer layer.
     */
    buffer_handle_t nativeHandle = UseGralloc() ? LoadNativeHandle(handle) : nullptr;
    /* fd-mapped buffers: flush CPU writes via DMA-BUF sync. */
    if (!nativeHandle) {
        if (handle.virAddr) {
            int bufFd = FindMappableFd(handle);
            if (bufFd >= 0) {
                DmaBufSync(bufFd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
                DmaBufSync(bufFd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
            }
        }
        return HDF_SUCCESS;
    }

    hybris_gralloc_unlock(nativeHandle);

    if (handle.virAddr) {
        void* vaddr = nullptr;
        int usage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
        hybris_gralloc_lock(nativeHandle, usage, 0, 0, handle.width, handle.height, &vaddr);
    }
    return HDF_SUCCESS;
}

/* ─── InvalidateCache ────────────────────────────────────────────────────── */

int32_t HybrisBufferVdiImpl::InvalidateCache(const BufferHandle& handle) const
{
    /* Re-locking invalidates the cache for subsequent CPU reads. */
    buffer_handle_t nativeHandle = UseGralloc() ? LoadNativeHandle(handle) : nullptr;
    /* fd-mapped buffers: invalidate via DMA-BUF sync for CPU reads. */
    if (!nativeHandle) {
        if (handle.virAddr) {
            int bufFd = FindMappableFd(handle);
            if (bufFd >= 0) {
                DmaBufSync(bufFd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            }
        }
        return HDF_SUCCESS;
    }

    hybris_gralloc_unlock(nativeHandle);

    if (handle.virAddr) {
        void* vaddr = nullptr;
        int usage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
        hybris_gralloc_lock(nativeHandle, usage, 0, 0, handle.width, handle.height, &vaddr);
    }
    return HDF_SUCCESS;
}

/* ─── IsSupportedAlloc ───────────────────────────────────────────────────── */

int32_t HybrisBufferVdiImpl::IsSupportedAlloc(
    const std::vector<VerifyAllocInfo>& infos,
    std::vector<bool>& supporteds) const
{
    /*
     * Return NOT_SUPPORT so RenderService falls back to its own capability
     * detection. We can implement proper querying later if needed.
     */
    (void)infos;
    (void)supporteds;
    return NOT_SUPPORT;
}

/* ─── Factory functions ──────────────────────────────────────────────────── */

extern "C" IDisplayBufferVdi* CreateDisplayBufferVdi()
{
    return new HybrisBufferVdiImpl();
}

extern "C" void DestroyDisplayBufferVdi(IDisplayBufferVdi* vdi)
{
    delete vdi;
}

} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS
