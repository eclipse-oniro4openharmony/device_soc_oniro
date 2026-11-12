/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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
#include "drm_allocator.h"
#include <cinttypes>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <securec.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "display_common.h"
#include "drm_fourcc.h"
namespace OHOS {
namespace HDI {
namespace DISPLAY {
int32_t DrmAllocator::Init()
{
    DISPLAY_LOGD();
    int32_t ret;
    drmFd_ = open(FILE_PATH, O_RDWR);
    DISPLAY_CHK_RETURN((drmFd_ < 0), DISPLAY_FAILURE,
        DISPLAY_LOGE("can not open drm file : %{public}s errno: %{public}d ", FILE_PATH, errno));
    ret = drmDropMaster(drmFd_);
    if (ret != 0) {
        DISPLAY_LOGW("can not drop master");
    }
    return DISPLAY_SUCCESS;
}

int32_t DrmAllocator::Allocate(const BufferInfo &bufferInfo, BufferHandle &handle)
{
    int32_t ret;
    int32_t fd;
    struct drm_mode_create_dumb dumb = {0};
    // create_dumb
    dumb.width = bufferInfo.widthStride_;
    dumb.height = bufferInfo.heightStride_;
    dumb.flags = 0;
    dumb.bpp = bufferInfo.bitsPerPixel_;
    // virtio-gpu only creates 32 bpp dumb buffers. Anything else (YUV camera
    // and video buffers, 16/24 bpp RGB) is backed by a 32 bpp buffer of the
    // same byte stride and at least the size the format needs; the planes are
    // laid out by the stride and size in the handle, not by the dumb buffer.
    constexpr uint32_t DUMB_BPP = 32;
    constexpr uint32_t DUMB_BYTES_PER_PIXEL = DUMB_BPP / 8;
    if (dumb.bpp != DUMB_BPP) {
        uint32_t rowBytes = bufferInfo.widthStride_ * bufferInfo.bytesPerPixel_;
        if (rowBytes % DUMB_BYTES_PER_PIXEL != 0 || rowBytes == 0) {
            DISPLAY_LOGE("unsupported row size %{public}u for format 0x%{public}x", rowBytes, bufferInfo.format_);
            return DISPLAY_NOT_SUPPORT;
        }
        dumb.width = rowBytes / DUMB_BYTES_PER_PIXEL;
        dumb.height = (bufferInfo.size_ + rowBytes - 1) / rowBytes;
        dumb.bpp = DUMB_BPP;
    }
    DISPLAY_LOGD();
    ret = drmIoctl(drmFd_, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
    DISPLAY_LOGI("fmt 0x%{public}x create dumb width: %{public}d  height: %{public}d bpp: %{public}u pitch %{public}d "
                 "size %{public}llu",
        handle.format, dumb.width, dumb.height, dumb.bpp, dumb.pitch, dumb.size);
    DISPLAY_CHK_RETURN(
        (ret != 0), DISPLAY_FAILURE, DISPLAY_LOGE("DRM_IOCTL_MODE_CREATE_DUMB failed errno %{public}d", errno));

    ret = drmPrimeHandleToFD(drmFd_, dumb.handle, DRM_CLOEXEC | DRM_RDWR, &fd);
    DISPLAY_CHK_RETURN(
        (ret != 0), DISPLAY_FAILURE, DISPLAY_LOGE("can not get fd from prime handle errno: %{public}d", errno));
    handle.fd = fd;
    handle.size = dumb.size;
    //handle.phyAddr = GetPhysicalAddr(handle.fd);
    // handle.format = bufferInfo.format_;    //PIXEL_FMT_BGRA_8888

    if ((handle.usage & HBM_USE_MEM_FB) != 0) {
        handle.format = PIXEL_FMT_BGRX_8888;
        DISPLAY_LOGI("drm Allocate set to PIXEL_FMT_BGRA_8888 for FB");
    }

    struct drm_mode_destroy_dumb destoryDumb = {0};
    destoryDumb.handle = dumb.handle;
    ret = drmIoctl(drmFd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destoryDumb);
    if (ret != 0) {
        DISPLAY_LOGW("can not destroy dumb errno: %{public}d", errno);
    }

    return DISPLAY_SUCCESS;
}

uint64_t DrmAllocator::GetPhysicalAddr(int primeFd)
{
    DISPLAY_LOGE("DRM_IOCTL_HISILICON_GEM_FD_TO_PHYADDR not support \n");
    return 0;
}

DrmAllocator::~DrmAllocator()
{
    DISPLAY_LOGD();
    if (drmFd_ >= 0) {
        close(drmFd_);
        drmFd_ = -1;
    }
}
} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS