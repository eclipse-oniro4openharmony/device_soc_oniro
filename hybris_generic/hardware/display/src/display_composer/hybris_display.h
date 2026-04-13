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

#ifndef HYBRIS_DISPLAY_H
#define HYBRIS_DISPLAY_H

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include "buffer_handle.h"
#include "v1_0/display_composer_type.h"
#include "common/include/display_common.h"
#include "hybris_layer.h"
#include <hybris/hwc2/hwc2_compatibility_layer.h>

namespace OHOS {
namespace HDI {
namespace DISPLAY {

using namespace OHOS::HDI::Display::Composer::V1_0;

/*
 * HybrisDisplay manages one physical display reported by the HWC2 HAL.
 * It holds the hwc2_compat_display_t* and owns a map of HybrisLayer objects.
 */
class HybrisDisplay {
public:
    HybrisDisplay(uint32_t devId, hwc2_compat_display_t* display, hwc2_display_t hwc2Id);
    ~HybrisDisplay();

    uint32_t GetDevId() const { return devId_; }
    hwc2_compat_display_t* GetHwc2Display() const { return display_; }
    hwc2_display_t GetHwc2DisplayId() const { return hwc2DisplayId_; }

    /* Layer management */
    int32_t CreateLayer(const LayerInfo& info, uint32_t& layerId);
    int32_t DestroyLayer(uint32_t layerId);
    HybrisLayer* GetLayer(uint32_t layerId);

    /* Display operations */
    int32_t GetDisplayCapability(DisplayCapability& info);
    int32_t GetDisplaySupportedModes(std::vector<DisplayModeInfo>& modes);
    int32_t GetDisplayMode(uint32_t& modeId);
    int32_t SetDisplayMode(uint32_t modeId);
    int32_t GetDisplayPowerStatus(DispPowerStatus& status);
    int32_t SetDisplayPowerStatus(DispPowerStatus status);

    int32_t SetDisplayVsyncEnabled(bool enabled);
    int32_t RegDisplayVBlankCallback(VBlankCallback cb, void* data);

    int32_t PrepareDisplayLayers(bool& needFlushFb);
    int32_t GetDisplayCompChange(std::vector<uint32_t>& layers, std::vector<int32_t>& types);
    int32_t SetDisplayClientBuffer(const BufferHandle& buffer, int32_t fence);
    int32_t GetDisplayReleaseFence(std::vector<uint32_t>& layers, std::vector<int32_t>& fences);
    int32_t Commit(int32_t& fence);

    /* Vsync callback — called from HWC2 event listener (static thunk) */
    void OnVsync(int64_t timestampNs);

private:
    uint32_t devId_{0};
    hwc2_compat_display_t* display_{nullptr};
    hwc2_display_t hwc2DisplayId_{0};
    std::unordered_map<uint32_t, std::unique_ptr<HybrisLayer>> layers_;
    uint32_t nextLayerId_{1};

    DispPowerStatus powerStatus_{POWER_STATUS_ON};

    VBlankCallback vblankCb_{nullptr};
    void* vblankData_{nullptr};

    /* Vsync sequence counter */
    uint32_t vsyncSeq_{0};

    /* Whether PrepareDisplayLayers found any CLIENT layers requiring flush */
    bool needsClientComposition_{false};

    /*
     * Layers the HAL requested as CLIENT this frame, populated by
     * getChangedCompositionTypes (if available) after acceptChanges.
     * Maps our OHOS layer ID → HWC2::Composition (as int32_t).
     * Empty when the full-CLIENT fallback is used instead.
     */
    std::unordered_map<uint32_t, int32_t> pendingClientLayers_;

    /*
     * Layers that are permanently CLIENT.
     *
     * The MTK HAL persistently requests CLIENT for certain layers (e.g. the
     * status bar, nav bar) on most frames.  When we report them as CLIENT via
     * GetDisplayCompChange, render_service destroys their DEVICE EGL window
     * surfaces and switches them to GPU composite mode.  On the NEXT frame,
     * if numTypes==0, the old code cleared pendingClientLayers_ and returned
     * empty from GetDisplayCompChange — causing render_service to transition
     * those layers BACK to DEVICE, which recreates DEVICE EGL surfaces.  The
     * HAL then requests CLIENT again next frame, destroying those surfaces while
     * RSRenderThread may be mid-eglSwapBuffers: the DEVICE↔CLIENT oscillation is
     * the root cause of the status-bar / nav-bar flicker.
     *
     * Fix: once a layer is observed as HAL-requested CLIENT, add it to
     * stickyClientLayers_ and keep reporting it as CLIENT on every subsequent
     * frame (even when numTypes==0).  render_service never transitions it back to
     * DEVICE, the HAL always sees CLIENT for it (numTypes==0), and the EGL
     * surface lifecycle stabilises: one DEVICE→CLIENT transition per session
     * (acceptable), zero ongoing oscillations (flicker eliminated).
     *
     * Entries are removed when the layer is destroyed (DestroyLayer).
     */
    std::unordered_map<uint32_t, int32_t> stickyClientLayers_;

    /* Out-fences from last present — released after Commit returns */
    hwc2_compat_out_fences_t* pendingFences_{nullptr};

    static int32_t OhosToHwc2PowerMode(DispPowerStatus status);
};

} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS

#endif // HYBRIS_DISPLAY_H
