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

#include "hybris_display.h"
#include <hdf_base.h>
#include <cstring>
#include <unordered_map>
#include "display_common.h"
#include <hardware/hwcomposer2.h>
#include <system/window.h>
#include <cutils/native_handle.h>
extern "C" {
#include <hybris/common/binding.h>
}

/*
 * Forward-declare the HWC2 C++ types we need for the getChangedCompositionTypes
 * dlsym call without pulling in the heavy HWC2.h (libui, libhardware, etc.).
 *
 * hwcomposer2.h defines the namespace HWC2 block only under HWC2_USE_CPP11,
 * which is not set in our build.  We redeclare the two types we need:
 *   - HWC2::Layer   (opaque class, used as pointer key only)
 *   - HWC2::Composition (enum class : int32_t, values from hwc2_composition_t)
 */
namespace HWC2 {
    class Layer;
    enum class Composition : int32_t {
        Invalid    = 0,  /* HWC2_COMPOSITION_INVALID    */
        Client     = 1,  /* HWC2_COMPOSITION_CLIENT     */
        Device     = 2,  /* HWC2_COMPOSITION_DEVICE     */
        SolidColor = 3,  /* HWC2_COMPOSITION_SOLID_COLOR */
        Cursor     = 4,  /* HWC2_COMPOSITION_CURSOR     */
        Sideband   = 5,  /* HWC2_COMPOSITION_SIDEBAND   */
    };
}

namespace OHOS {
namespace HDI {
namespace DISPLAY {

HybrisDisplay::HybrisDisplay(uint32_t devId, hwc2_compat_display_t* display, hwc2_display_t hwc2Id)
    : devId_(devId), display_(display), hwc2DisplayId_(hwc2Id)
{
    /* Turn on vsync immediately so we get timing from the start */
    hwc2_compat_display_set_vsync_enabled(display_, HWC2_VSYNC_ENABLE);
    hwc2_compat_display_set_power_mode(display_, HWC2_POWER_MODE_ON);
}

HybrisDisplay::~HybrisDisplay()
{
    if (pendingFences_) {
        hwc2_compat_out_fences_destroy(pendingFences_);
        pendingFences_ = nullptr;
    }
    /* Layers are owned by unique_ptr — destroyed automatically */
}

/* ── Layer management ─────────────────────────────────────────────────────── */

int32_t HybrisDisplay::CreateLayer(const LayerInfo& info, uint32_t& layerId)
{
    hwc2_compat_layer_t* hwc2Layer = hwc2_compat_display_create_layer(display_);
    DISPLAY_CHK_RETURN(hwc2Layer == nullptr, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_display_create_layer failed for display %u", devId_));

    layerId = nextLayerId_++;
    auto layer = std::make_unique<HybrisLayer>(hwc2Layer, layerId);

    /* Apply initial composition type based on layer type hint */
    CompositionType compType = COMPOSITION_DEVICE;
    if (info.type == LAYER_TYPE_CURSOR) {
        compType = COMPOSITION_CURSOR;
    }
    layer->SetLayerCompositionType(compType);

    /* Initial display frame covering the whole display */
    HWC2DisplayConfig* cfg = hwc2_compat_display_get_active_config(display_);
    if (cfg) {
        IRect region = {0, 0, cfg->width, cfg->height};
        layer->SetLayerRegion(region);
        layer->SetLayerCrop(region);
        free(cfg);
    }

    layers_[layerId] = std::move(layer);
    DISPLAY_LOGI("Created layer %u on display %u", layerId, devId_);
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::DestroyLayer(uint32_t layerId)
{
    auto it = layers_.find(layerId);
    DISPLAY_CHK_RETURN(it == layers_.end(), HDF_FAILURE,
        DISPLAY_LOGE("Layer %u not found on display %u", layerId, devId_));

    hwc2_compat_display_destroy_layer(display_, it->second->GetHwc2Layer());
    layers_.erase(it);
    stickyClientLayers_.erase(layerId);
    pendingClientLayers_.erase(layerId);
    DISPLAY_LOGI("Destroyed layer %u on display %u", layerId, devId_);
    return HDF_SUCCESS;
}

HybrisLayer* HybrisDisplay::GetLayer(uint32_t layerId)
{
    auto it = layers_.find(layerId);
    if (it == layers_.end()) {
        return nullptr;
    }
    return it->second.get();
}

/* ── Display capability / mode queries ───────────────────────────────────── */

int32_t HybrisDisplay::GetDisplayCapability(DisplayCapability& info)
{
    info.name = "hybris-hwc2-display";
    info.type = DISP_INTF_MIPI; /* Volla X23 has an internal MIPI DSI panel */
    info.supportLayers = 16;
    info.virtualDispCount = 0;
    info.supportWriteBack = false;
    info.propertyCount = 0;

    /*
     * phyWidth / phyHeight are the physical screen dimensions in millimetres.
     * AbstractDisplay::CalculateXYDpi() uses them to compute xDpi/yDpi:
     *   xDpi = pixelWidth * 25.4 / phyWidth_mm
     * Volla X23: 6.3" diagonal, 720×1560 → ~67 mm × 145 mm → ~272 DPI.
     * (The pixel resolution from the active HWC2 config is NOT used here.)
     */
    info.phyWidth  = 65;   /* mm */
    info.phyHeight = 141;  /* mm */
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::GetDisplaySupportedModes(std::vector<DisplayModeInfo>& modes)
{
    HWC2DisplayConfig* cfg = hwc2_compat_display_get_active_config(display_);
    if (!cfg) {
        DISPLAY_LOGE("No active config for display %u", devId_);
        return HDF_FAILURE;
    }

    DisplayModeInfo mode;
    mode.width = cfg->width;
    mode.height = cfg->height;
    /* vsyncPeriod is in nanoseconds; convert to Hz */
    mode.freshRate = (cfg->vsyncPeriod > 0) ?
        static_cast<uint32_t>(1000000000LL / cfg->vsyncPeriod) : 60u;
    mode.id = static_cast<int32_t>(cfg->id);

    modes.push_back(mode);
    free(cfg);
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::GetDisplayMode(uint32_t& modeId)
{
    HWC2DisplayConfig* cfg = hwc2_compat_display_get_active_config(display_);
    DISPLAY_CHK_RETURN(cfg == nullptr, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_display_get_active_config failed"));
    modeId = static_cast<uint32_t>(cfg->id);
    free(cfg);
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::SetDisplayMode(uint32_t modeId)
{
    /* HWC2 compat layer doesn't expose setActiveConfig; mode changes are
     * not supported in this bring-up implementation. */
    DISPLAY_UNUSED(modeId);
    DISPLAY_LOGW("SetDisplayMode not supported in hybris VDI");
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::GetDisplayPowerStatus(DispPowerStatus& status)
{
    status = powerStatus_;
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::SetDisplayPowerStatus(DispPowerStatus status)
{
    DISPLAY_LOGI("HybrisDisplay::SetDisplayPowerStatus devId=%u status=%d", devId_, status);
    int hwc2Mode = OhosToHwc2PowerMode(status);
    hwc2_error_t err = hwc2_compat_display_set_power_mode(display_, hwc2Mode);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_display_set_power_mode failed: %d", err));
    powerStatus_ = status;
    return HDF_SUCCESS;
}

/* ── Vsync ────────────────────────────────────────────────────────────────── */

int32_t HybrisDisplay::SetDisplayVsyncEnabled(bool enabled)
{
    DISPLAY_LOGI("HybrisDisplay::SetDisplayVsyncEnabled devId=%u enabled=%d", devId_, enabled);
    int hwc2Enabled = enabled ? HWC2_VSYNC_ENABLE : HWC2_VSYNC_DISABLE;
    hwc2_error_t err = hwc2_compat_display_set_vsync_enabled(display_, hwc2Enabled);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_display_set_vsync_enabled failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::RegDisplayVBlankCallback(VBlankCallback cb, void* data)
{
    vblankCb_ = cb;
    vblankData_ = data;
    return HDF_SUCCESS;
}

void HybrisDisplay::OnVsync(int64_t timestampNs)
{
    if (vblankCb_) {
        vblankCb_(vsyncSeq_++, static_cast<uint64_t>(timestampNs), vblankData_);
    }
}

/* ── Composition ─────────────────────────────────────────────────────────── */

/*
 * HWC2::Display::getChangedCompositionTypes function pointer.
 *
 * This C++ method exists in the prebuilt libhwc2_compat_layer.so but has no
 * C wrapper.  We load it once via android_dlsym using its mangled name.
 *
 * Calling convention (ARM64 AAPCS):
 *   x0 = this (HWC2::Display*)
 *   x1 = outTypes (std::unordered_map<HWC2::Layer*, HWC2::Composition>*)
 *   return x0 = HWC2::Error (int32_t, 0 = None)
 *
 * ABI assumption: both Android 12 and OHOS 6.1 use LLVM libc++ (std::__1).
 * The std::__1::unordered_map internal layout is stable across LLVM versions
 * 12–17, and libhybris hooks malloc/free so both sides share one heap.
 */
using GetChangedTypesFn = int32_t(*)(void* displaySelf,
    std::unordered_map<HWC2::Layer*, HWC2::Composition>* outTypes);

static GetChangedTypesFn LoadGetChangedTypesFn()
{
    void* handle = android_dlopen("libhwc2_compat_layer.so", RTLD_LAZY);
    if (!handle) {
        DISPLAY_LOGW("android_dlopen libhwc2_compat_layer.so failed");
        return nullptr;
    }
    auto fn = reinterpret_cast<GetChangedTypesFn>(android_dlsym(handle,
        "_ZN4HWC27Display26getChangedCompositionTypesEPNSt3__113unordered_mapIPNS_5LayerENS_"
        "11CompositionENS1_4hashIS4_EENS1_8equal_toIS4_EENS1_9allocatorINS1_4pairIKS4_S5_EEEEEE"));
    DISPLAY_LOGI("getChangedCompositionTypes symbol: %s", fn ? "FOUND" : "NOT FOUND");
    return fn;
}

int32_t HybrisDisplay::PrepareDisplayLayers(bool& needFlushFb)
{
    uint32_t numTypes = 0;
    uint32_t numRequests = 0;
    hwc2_error_t err = hwc2_compat_display_validate(display_, &numTypes, &numRequests);

    if (err != HWC2_ERROR_HAS_CHANGES && err != HWC2_ERROR_NONE) {
        DISPLAY_LOGE("hwc2_compat_display_validate failed: %d", err);
        needFlushFb = true;
        return HDF_FAILURE;
    }

    /*
     * Accept HAL composition changes immediately after validate, before
     * returning to render_service.  This keeps the accept call spec-compliant:
     * render_service's SetLayerCompositionType calls happen after accept (allowed),
     * not between validate and accept (forbidden by HWC2 spec).
     */
    hwc2_compat_display_accept_changes(display_);

    if (numTypes == 0) {
        if (stickyClientLayers_.empty()) {
            /* Pure DEVICE steady-state — no GPU composite needed */
            DISPLAY_LOGI("PrepareDisplayLayers devId=%u: numTypes=0 numRequests=%u (DEVICE)",
                devId_, numRequests);
            needFlushFb = false;
            needsClientComposition_ = false;
            pendingClientLayers_.clear();
        } else {
            /*
             * HAL agrees with the current composition types (numTypes==0).
             * Because render_service already set the sticky CLIENT layers to CLIENT
             * on the previous frame, the HAL sees CLIENT for them and returns
             * numTypes==0.  We must keep reporting them as CLIENT and keep
             * needFlushFb=true so render_service continues to GPU-composite and
             * call SetDisplayClientBuffer — otherwise the HAL would present a stale
             * client target.
             */
            DISPLAY_LOGI("PrepareDisplayLayers devId=%u: numTypes=0 numRequests=%u "
                "(%zu sticky CLIENT layer(s) — maintaining)",
                devId_, numRequests, stickyClientLayers_.size());
            pendingClientLayers_ = stickyClientLayers_;
            needsClientComposition_ = true;
            needFlushFb = true;
        }
        return HDF_SUCCESS;
    }

    /*
     * numTypes > 0: HAL wants to change some layers' composition type.
     *
     * Use getChangedCompositionTypes (dlsym'd from the prebuilt .so) to find
     * exactly which layers the HAL changed to CLIENT.  Add them to
     * stickyClientLayers_ — from this point on they stay CLIENT permanently
     * (until destroyed).  This prevents the DEVICE↔CLIENT oscillation that
     * causes the EGL surface destruction race / status-bar flicker.
     *
     * Fallback (symbol unavailable): pendingClientLayers_ stays empty, which
     * suppresses layer transitions and avoids the HAL freeze from a missing
     * SetDisplayClientBuffer (stale client target risk, but no worse than before).
     */
    static GetChangedTypesFn s_fn = LoadGetChangedTypesFn();

    if (s_fn) {
        /*
         * Read HWC2::Display* from the opaque hwc2_compat_display_t.
         * struct hwc2_compat_display { HWC2::Display* self; } — first field.
         */
        void* displaySelf = *reinterpret_cast<void**>(display_);

        std::unordered_map<HWC2::Layer*, HWC2::Composition> changedTypes;
        int32_t fnErr = s_fn(displaySelf, &changedTypes);

        if (fnErr == 0 /* HWC2::Error::None */) {
            /*
             * Reverse-map HWC2::Layer* → our layer ID.
             * struct hwc2_compat_layer_t { HWC2::Layer* self; } — first field.
             */
            for (const auto& entry : changedTypes) {
                void* hwc2LayerPtr = entry.first;
                HWC2::Composition comp = entry.second;
                for (const auto& kv : layers_) {
                    void* layerSelf = *reinterpret_cast<void**>(kv.second->GetHwc2Layer());
                    if (layerSelf == hwc2LayerPtr) {
                        uint32_t layerId = kv.first;
                        int32_t compInt = static_cast<int32_t>(comp);
                        bool isNew = (stickyClientLayers_.find(layerId) == stickyClientLayers_.end());
                        stickyClientLayers_[layerId] = compInt;
                        DISPLAY_LOGW("PrepareDisplayLayers devId=%u: layer %u → "
                            "composition %d (%s sticky CLIENT)",
                            devId_, layerId, compInt, isNew ? "NEW" : "already");
                        break;
                    }
                }
            }
            DISPLAY_LOGW("PrepareDisplayLayers devId=%u: %u HAL-changed layer(s); "
                "%zu total sticky CLIENT layer(s)",
                devId_, static_cast<uint32_t>(changedTypes.size()),
                stickyClientLayers_.size());
        } else {
            DISPLAY_LOGW("PrepareDisplayLayers devId=%u: getChangedCompositionTypes "
                "returned error %d", devId_, fnErr);
        }
    } else {
        DISPLAY_LOGW("PrepareDisplayLayers devId=%u: %u type change(s), "
            "getChangedCompositionTypes unavailable — suppressing layer transitions",
            devId_, numTypes);
    }

    /* Report all sticky CLIENT layers (new ones trigger one-time transition) */
    pendingClientLayers_ = stickyClientLayers_;
    needsClientComposition_ = !pendingClientLayers_.empty();
    needFlushFb = true;  /* always provide client target when numTypes > 0 */
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::GetDisplayCompChange(std::vector<uint32_t>& layerIds,
    std::vector<int32_t>& types)
{
    layerIds.clear();
    types.clear();

    if (!needsClientComposition_ || pendingClientLayers_.empty()) {
        /*
         * Either steady-state DEVICE (numTypes==0) or fallback mode where we
         * suppress all layer transitions to avoid the EGL surface destruction race.
         */
        return HDF_SUCCESS;
    }

    /*
     * Report exactly the layers the HAL changed to CLIENT (from
     * getChangedCompositionTypes).  render_service will call
     * SetLayerCompositionType(CLIENT) only for these layers and
     * GPU-composite them into the client target.  All other layers keep
     * their DEVICE EGL window surfaces — no destruction race.
     */
    for (const auto& entry : pendingClientLayers_) {
        uint32_t layerId = entry.first;
        int32_t comp     = entry.second;
        /* Only report CLIENT transitions — Device/Cursor etc. are already set */
        if (comp == static_cast<int32_t>(HWC2::Composition::Client)) {
            layerIds.push_back(layerId);
            types.push_back(COMPOSITION_CLIENT);
        }
    }
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::SetDisplayClientBuffer(const BufferHandle& buffer, int32_t fence)
{
    /*
     * Build an ANativeWindowBuffer from the OHOS BufferHandle so we can pass
     * it to hwc2_compat_display_set_client_target.
     */
    int numFds = 1 + buffer.reserveFds;
    /* We added 2 ints (kPtrSlots) for the native pointer in our buffer VDI. Exclude them for HWC2. */
    int numInts = (buffer.reserveInts >= 2) ? (buffer.reserveInts - 2) : buffer.reserveInts;

    native_handle_t* nh = native_handle_create(numFds, numInts);
    DISPLAY_CHK_RETURN(nh == nullptr, HDF_FAILURE,
        DISPLAY_LOGE("native_handle_create failed for client buffer"));

    nh->data[0] = buffer.fd;
    for (uint32_t i = 0; i < buffer.reserveFds; i++) {
        nh->data[1 + i] = buffer.reserve[i];
    }
    for (int i = 0; i < numInts; i++) {
        nh->data[numFds + i] = buffer.reserve[buffer.reserveFds + i];
    }

    ANativeWindowBuffer nb;
    memset(&nb, 0, sizeof(nb));
    nb.common.magic   = ANDROID_NATIVE_BUFFER_MAGIC;
    nb.common.version = sizeof(ANativeWindowBuffer);
    nb.width  = buffer.width;
    nb.height = buffer.height;
    nb.stride = buffer.stride;
    nb.format = buffer.format;
    nb.usage  = static_cast<uint64_t>(buffer.usage);
    nb.layerCount = 1;
    nb.handle = nh;

    hwc2_error_t err = hwc2_compat_display_set_client_target(display_,
        0, &nb, fence, HAL_DATASPACE_UNKNOWN);

    // DO NOT call native_handle_close(nh). The FDs inside belong to the OHOS
    // BufferHandle and are still in use.
    native_handle_delete(nh);

    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_display_set_client_target failed: %d", err));
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::GetDisplayReleaseFence(std::vector<uint32_t>& layerIds,
    std::vector<int32_t>& fences)
{
    if (!pendingFences_) {
        layerIds.clear();
        fences.clear();
        return HDF_SUCCESS;
    }

    for (auto& kv : layers_) {
        int32_t fd = hwc2_compat_out_fences_get_fence(pendingFences_,
            kv.second->GetHwc2Layer());
        if (fd >= 0) {
            layerIds.push_back(kv.first);
            fences.push_back(fd);
        }
    }

    hwc2_compat_out_fences_destroy(pendingFences_);
    pendingFences_ = nullptr;
    return HDF_SUCCESS;
}

int32_t HybrisDisplay::Commit(int32_t& fence)
{
    /* acceptDisplayChanges was already called in PrepareDisplayLayers */

    if (pendingFences_) {
        hwc2_compat_out_fences_destroy(pendingFences_);
        pendingFences_ = nullptr;
    }

    hwc2_error_t err = hwc2_compat_display_get_release_fences(display_, &pendingFences_);
    if (err != HWC2_ERROR_NONE) {
        DISPLAY_LOGW("hwc2_compat_display_get_release_fences failed: %d", err);
    }

    int32_t presentFence = -1;
    err = hwc2_compat_display_present(display_, &presentFence);
    DISPLAY_LOGI("HybrisDisplay::Commit devId=%u presentFence=%d err=%d", devId_, presentFence, err);
    DISPLAY_CHK_RETURN(err != HWC2_ERROR_NONE, HDF_FAILURE,
        DISPLAY_LOGE("hwc2_compat_display_present failed: %d", err));

    fence = presentFence;
    return HDF_SUCCESS;
}

/* ── Static helpers ──────────────────────────────────────────────────────── */

int32_t HybrisDisplay::OhosToHwc2PowerMode(DispPowerStatus status)
{
    switch (status) {
        case POWER_STATUS_ON:      return HWC2_POWER_MODE_ON;
        case POWER_STATUS_STANDBY: return HWC2_POWER_MODE_DOZE;
        case POWER_STATUS_SUSPEND: return HWC2_POWER_MODE_DOZE_SUSPEND;
        case POWER_STATUS_OFF:     return HWC2_POWER_MODE_OFF;
        default:                   return HWC2_POWER_MODE_ON;
    }
}

} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS
