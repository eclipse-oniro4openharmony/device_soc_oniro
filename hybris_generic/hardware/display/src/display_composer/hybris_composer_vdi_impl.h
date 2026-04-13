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

#ifndef HYBRIS_COMPOSER_VDI_IMPL_H
#define HYBRIS_COMPOSER_VDI_IMPL_H

#include <mutex>
#include <unordered_map>
#include <memory>
#include "idisplay_composer_vdi.h"
#include "v1_0/display_composer_type.h"
#include "hybris_display.h"
#include <hybris/hwc2/hwc2_compatibility_layer.h>

namespace OHOS {
namespace HDI {
namespace DISPLAY {

using namespace OHOS::HDI::Display::Composer;
using namespace OHOS::HDI::Display::Composer::V1_0;

/*
 * HybrisComposerVdiImpl — implements IDisplayComposerVdi by delegating to
 * Android HWC2 HIDL service via the libhybris hwc2_compat_* C wrappers.
 *
 * One instance is created per composer_host process lifetime (singleton
 * returned by GetVdiInstance(), but CreateComposerVdi() allocates a fresh
 * instance for the factory-function path used by the HDI service).
 */
class HybrisComposerVdiImpl : public IDisplayComposerVdi {
public:
    static HybrisComposerVdiImpl& GetVdiInstance();
    HybrisComposerVdiImpl();
    ~HybrisComposerVdiImpl() override;

    /* IDisplayComposerVdi */
    int32_t RegHotPlugCallback(HotPlugCallback cb, void* data) override;
    int32_t GetDisplayCapability(uint32_t devId, DisplayCapability& info) override;
    int32_t GetDisplaySupportedModes(uint32_t devId, std::vector<DisplayModeInfo>& modes) override;
    int32_t GetDisplayMode(uint32_t devId, uint32_t& modeId) override;
    int32_t SetDisplayMode(uint32_t devId, uint32_t modeId) override;
    int32_t GetDisplayPowerStatus(uint32_t devId, DispPowerStatus& status) override;
    int32_t SetDisplayPowerStatus(uint32_t devId, DispPowerStatus status) override;
    int32_t GetDisplayBacklight(uint32_t devId, uint32_t& level) override;
    int32_t SetDisplayBacklight(uint32_t devId, uint32_t level) override;
    int32_t GetDisplayProperty(uint32_t devId, uint32_t id, uint64_t& value) override;
    int32_t GetDisplayCompChange(uint32_t devId,
        std::vector<uint32_t>& layers, std::vector<int32_t>& types) override;
    int32_t SetDisplayClientCrop(uint32_t devId, const IRect& rect) override;
    int32_t SetDisplayClientBuffer(uint32_t devId, const BufferHandle& buffer, int32_t fence) override;
    int32_t SetDisplayClientDamage(uint32_t devId, std::vector<IRect>& rects) override;
    int32_t SetDisplayVsyncEnabled(uint32_t devId, bool enabled) override;
    int32_t RegDisplayVBlankCallback(uint32_t devId, VBlankCallback cb, void* data) override;
    int32_t GetDisplayReleaseFence(uint32_t devId,
        std::vector<uint32_t>& layers, std::vector<int32_t>& fences) override;
    int32_t CreateVirtualDisplay(uint32_t width, uint32_t height,
        int32_t& format, uint32_t& devId) override;
    int32_t DestroyVirtualDisplay(uint32_t devId) override;
    int32_t SetVirtualDisplayBuffer(uint32_t devId, const BufferHandle& buffer,
        const int32_t fence) override;
    int32_t SetDisplayProperty(uint32_t devId, uint32_t id, uint64_t value) override;
    int32_t Commit(uint32_t devId, int32_t& fence) override;
    int32_t CreateLayer(uint32_t devId, const LayerInfo& layerInfo, uint32_t& layerId) override;
    int32_t DestroyLayer(uint32_t devId, uint32_t layerId) override;
    int32_t PrepareDisplayLayers(uint32_t devId, bool& needFlushFb) override;
    int32_t SetLayerAlpha(uint32_t devId, uint32_t layerId, const LayerAlpha& alpha) override;
    int32_t SetLayerRegion(uint32_t devId, uint32_t layerId, const IRect& rect) override;
    int32_t SetLayerCrop(uint32_t devId, uint32_t layerId, const IRect& rect) override;
    int32_t SetLayerZorder(uint32_t devId, uint32_t layerId, uint32_t zorder) override;
    int32_t SetLayerPreMulti(uint32_t devId, uint32_t layerId, bool preMul) override;
    int32_t SetLayerTransformMode(uint32_t devId, uint32_t layerId, TransformType type) override;
    int32_t SetLayerDirtyRegion(uint32_t devId, uint32_t layerId,
        const std::vector<IRect>& rects) override;
    int32_t SetLayerVisibleRegion(uint32_t devId, uint32_t layerId,
        std::vector<IRect>& rects) override;
    int32_t SetLayerBuffer(uint32_t devId, uint32_t layerId,
        const BufferHandle& buffer, int32_t fence) override;
    int32_t SetLayerCompositionType(uint32_t devId, uint32_t layerId,
        CompositionType type) override;
    int32_t SetLayerBlendType(uint32_t devId, uint32_t layerId, BlendType type) override;
    int32_t SetLayerMaskInfo(uint32_t devId, uint32_t layerId, const MaskInfo maskInfo) override;
    int32_t SetLayerColor(uint32_t devId, uint32_t layerId, const LayerColor& layerColor) override;

private:
    /* Lookup helpers — return nullptr on failure */
    HybrisDisplay* GetDisplay(uint32_t devId);
    HybrisLayer* GetLayer(uint32_t devId, uint32_t layerId);

    /* Initialise the HWC2 device and register callbacks. Called once from ctor. */
    void InitHwc2Device();

    /* Pre-load gralloc mapper so android_load_sphal_library succeeds (same fix
     * as test_hwcomposer). Called from InitHwc2Device(). */
    void PreloadGrallocMapper();

    /* Static callbacks forwarded from HWC2EventListener */
    static void OnHotplug(HWC2EventListener* self, int32_t seq,
        hwc2_display_t display, bool connected, bool primary);
    static void OnVsync(HWC2EventListener* self, int32_t seq,
        hwc2_display_t display, int64_t timestamp);
    static void OnRefresh(HWC2EventListener* self, int32_t seq,
        hwc2_display_t display);

    /* Called with mutex_ held */
    void HandleHotplug(hwc2_display_t hwc2Id, bool connected);

    hwc2_compat_device_t* device_{nullptr};
    HWC2EventListener eventListener_{};
    int composerSeq_{0};

    std::mutex mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<HybrisDisplay>> displays_;
    uint32_t nextDevId_{0};

    HotPlugCallback hotplugCb_{nullptr};
    void* hotplugData_{nullptr};
};

extern "C" int32_t GetDumpInfo(std::string& result);
extern "C" int32_t UpdateConfig(std::string& result);

} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS

#endif // HYBRIS_COMPOSER_VDI_IMPL_H
