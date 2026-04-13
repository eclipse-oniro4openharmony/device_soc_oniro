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

#include "hybris_layer.h"
#include <hdf_base.h>
#include <cstring>
#include <cstdlib>
#include "display_common.h"

/* Android HWC2 / hardware headers pulled in via android-headers */
#include <hardware/hwcomposer2.h>
#include <system/window.h>     /* ANativeWindowBuffer, native_handle_t */
#include <cutils/native_handle.h>

namespace OHOS {
namespace HDI {
namespace DISPLAY {

HybrisLayer::HybrisLayer(hwc2_compat_layer_t* layer, uint32_t id)
    : layer_(layer), id_(id)
{
}

/*
 * Build a minimal ANativeWindowBuffer wrapping an OHOS BufferHandle so that
 * the Android HWC2 HAL can consume it.
 *
 * OHOS BufferHandle layout:
 *   fd           — primary dma-buf fd
 *   reserveFds   — number of additional fds in reserve[]
 *   reserveInts  — number of ints in reserve[] after the fds
 *   reserve[]    — [0..reserveFds-1] extra fds, [reserveFds..] ints
 *
 * native_handle_t layout:
 *   data[0..numFds-1]          — file descriptors
 *   data[numFds..numFds+numInts-1] — ints
 */
struct HybrisNativeBuffer {
    /* Must be first — hwc2_compat_layer_set_buffer takes ANativeWindowBuffer* */
    ANativeWindowBuffer buf;

    /* The native_handle_t we construct lives right after (variable size) */
    /* Actual storage for the handle is heap-allocated and kept here */
    native_handle_t* handle{nullptr};

    ~HybrisNativeBuffer()
    {
        if (handle) {
            native_handle_close(handle);
            native_handle_delete(handle);
            handle = nullptr;
        }
    }
};

static HybrisNativeBuffer* BuildNativeBuffer(const BufferHandle& bh)
{
    int numFds = 1 + bh.reserveFds;
    int numInts = bh.reserveInts;

    native_handle_t* nh = native_handle_create(numFds, numInts);
    if (!nh) {
        DISPLAY_LOGE("native_handle_create failed (numFds=%d, numInts=%d)", numFds, numInts);
        return nullptr;
    }

    /* Primary fd */
    nh->data[0] = bh.fd;
    /* Additional reserve fds */
    for (uint32_t i = 0; i < bh.reserveFds; i++) {
        nh->data[1 + static_cast<int>(i)] = bh.reserve[i];
    }
    /* Reserve ints */
    for (uint32_t i = 0; i < bh.reserveInts; i++) {
        nh->data[numFds + static_cast<int>(i)] = bh.reserve[bh.reserveFds + i];
    }

    auto* nb = new HybrisNativeBuffer();
    memset(&nb->buf, 0, sizeof(nb->buf));

    nb->buf.common.magic   = ANDROID_NATIVE_BUFFER_MAGIC;
    nb->buf.common.version = sizeof(ANativeWindowBuffer);
    /* incRef / decRef left null — HWC2 compat layer doesn't call them */
    nb->buf.width  = bh.width;
    nb->buf.height = bh.height;
    nb->buf.stride = bh.stride;
    nb->buf.format = bh.format; /* OHOS formats align with Android for common cases */
    nb->buf.usage  = static_cast<uint64_t>(bh.usage);
    nb->buf.layerCount = 1;
    nb->buf.handle = nh;
    nb->handle = nh;

    return nb;
}

int32_t HybrisLayer::SetLayerAlpha(const LayerAlpha& alpha)
{
    /* HWC2 has a single plane alpha (0.0–1.0). Use global alpha when set. */
    float planeAlpha = alpha.enGlobalAlpha ? (alpha.gAlpha / 255.0f) : 1.0f;
    hwc2_error_t err = hwc2_compat_layer_set_plane_alpha(layer_, planeAlpha);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_layer_set_plane_alpha failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerRegion(const IRect& rect)
{
    hwc2_error_t err = hwc2_compat_layer_set_display_frame(layer_,
        rect.x, rect.y, rect.x + rect.w, rect.y + rect.h);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_layer_set_display_frame failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerCrop(const IRect& rect)
{
    hwc2_error_t err = hwc2_compat_layer_set_source_crop(layer_,
        static_cast<float>(rect.x), static_cast<float>(rect.y),
        static_cast<float>(rect.x + rect.w), static_cast<float>(rect.y + rect.h));
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_layer_set_source_crop failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerZorder(uint32_t zorder)
{
    DISPLAY_UNUSED(zorder);
    /* HWC2 manages z-order via layer ordering; no direct per-layer API.
     * RenderService handles z-ordering at composition time. */
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerPreMulti(bool preMul)
{
    int blendMode = preMul ? HWC2_BLEND_MODE_PREMULTIPLIED : HWC2_BLEND_MODE_NONE;
    hwc2_error_t err = hwc2_compat_layer_set_blend_mode(layer_, blendMode);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_layer_set_blend_mode (preMulti) failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerTransformMode(TransformType type)
{
    int transform = OhosToHwc2Transform(type);
    hwc2_error_t err = hwc2_compat_layer_set_transform(layer_, transform);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_layer_set_transform failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerDirtyRegion(const std::vector<IRect>& rects)
{
    DISPLAY_UNUSED(rects);
    /* Dirty region is advisory; HWC2 doesn't have a direct API for this.
     * The visible region is the relevant concept in HWC2. */
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerBuffer(const BufferHandle& buffer, int32_t fence)
{
    HybrisNativeBuffer* nb = BuildNativeBuffer(buffer);
    if (!nb) {
        DISPLAY_LOGE("BuildNativeBuffer failed for layer %u", id_);
        return HDF_FAILURE;
    }

    hwc2_error_t err = hwc2_compat_layer_set_buffer(layer_, 0, &nb->buf, fence);
    delete nb;

    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_layer_set_buffer failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerCompositionType(CompositionType type)
{
    compType_ = type;
    int hwc2Type = OhosToHwc2CompositionType(type);
    hwc2_error_t err = hwc2_compat_layer_set_composition_type(layer_, hwc2Type);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_layer_set_composition_type failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerBlendType(BlendType type)
{
    int blendMode = OhosToHwc2BlendMode(type);
    hwc2_error_t err = hwc2_compat_layer_set_blend_mode(layer_, blendMode);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_layer_set_blend_mode failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisLayer::SetLayerColor(const LayerColor& color)
{
    hwc_color_t hwcColor;
    hwcColor.r = color.r;
    hwcColor.g = color.g;
    hwcColor.b = color.b;
    hwcColor.a = color.a;
    hwc2_error_t err = hwc2_compat_layer_set_color(layer_, hwcColor);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_layer_set_color failed: %d", err));
    return HDF_SUCCESS;
}

/* ── Type mapping helpers ─────────────────────────────────────────────────── */

int32_t HybrisLayer::OhosToHwc2CompositionType(CompositionType type)
{
    switch (type) {
        case COMPOSITION_CLIENT:       return HWC2_COMPOSITION_CLIENT;
        case COMPOSITION_DEVICE:       return HWC2_COMPOSITION_DEVICE;
        case COMPOSITION_CURSOR:       return HWC2_COMPOSITION_CURSOR;
        case COMPOSITION_VIDEO:        return HWC2_COMPOSITION_DEVICE; /* no direct HWC2 equivalent */
        case COMPOSITION_DEVICE_CLEAR: return HWC2_COMPOSITION_DEVICE;
        case COMPOSITION_CLIENT_CLEAR: return HWC2_COMPOSITION_CLIENT;
        case COMPOSITION_TUNNEL:       return HWC2_COMPOSITION_SIDEBAND;
        default:
            DISPLAY_LOGW("Unknown CompositionType %d, using CLIENT", type);
            return HWC2_COMPOSITION_CLIENT;
    }
}

int32_t HybrisLayer::OhosToHwc2BlendMode(BlendType type)
{
    switch (type) {
        case BLEND_NONE:     return HWC2_BLEND_MODE_NONE;
        case BLEND_SRCOVER:  return HWC2_BLEND_MODE_PREMULTIPLIED;
        case BLEND_SRC:      return HWC2_BLEND_MODE_PREMULTIPLIED;
        default:             return HWC2_BLEND_MODE_PREMULTIPLIED;
    }
}

int32_t HybrisLayer::OhosToHwc2Transform(TransformType type)
{
    switch (type) {
        case ROTATE_NONE:        return 0; /* HAL_TRANSFORM_ROT_0 == 0 */
        case ROTATE_90:          return HWC_TRANSFORM_ROT_90;
        case ROTATE_180:         return HWC_TRANSFORM_ROT_180;
        case ROTATE_270:         return HWC_TRANSFORM_ROT_270;
        case MIRROR_H:           return HWC_TRANSFORM_FLIP_H;
        case MIRROR_V:           return HWC_TRANSFORM_FLIP_V;
        case MIRROR_H_ROTATE_90: return HWC_TRANSFORM_FLIP_H_ROT_90;
        case MIRROR_V_ROTATE_90: return HWC_TRANSFORM_FLIP_V_ROT_90;
        default:                 return 0;
    }
}

} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS
