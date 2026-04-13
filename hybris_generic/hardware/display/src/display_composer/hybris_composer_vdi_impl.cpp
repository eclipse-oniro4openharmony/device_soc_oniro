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

#include "hybris_composer_vdi_impl.h"
#include <hdf_base.h>
#include <dlfcn.h>
#include "display_common.h"

/* android_dlopen from libhybris */
extern "C" void* android_dlopen(const char* filename, int flags);

namespace OHOS {
namespace HDI {
namespace DISPLAY {

/* ── Singleton ───────────────────────────────────────────────────────────── */

HybrisComposerVdiImpl& HybrisComposerVdiImpl::GetVdiInstance()
{
    static HybrisComposerVdiImpl instance;
    return instance;
}

/* ── Constructor / destructor ────────────────────────────────────────────── */

HybrisComposerVdiImpl::HybrisComposerVdiImpl()
{
    InitHwc2Device();
}

HybrisComposerVdiImpl::~HybrisComposerVdiImpl()
{
    /* device_ is owned by the HWC2 compat layer; no explicit free needed */
}

/* ── Initialisation ──────────────────────────────────────────────────────── */

void HybrisComposerVdiImpl::PreloadGrallocMapper()
{
    static const char* kMapperPaths[] = {
        "/android/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so",
        "/android/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl.so",
        nullptr,
    };
    for (int i = 0; kMapperPaths[i] != nullptr; i++) {
        void* h = android_dlopen(kMapperPaths[i], RTLD_LAZY | RTLD_GLOBAL);
        if (h) {
            DISPLAY_LOGI("Pre-loaded gralloc mapper: %s", kMapperPaths[i]);
            return;
        }
    }
    DISPLAY_LOGW("Could not pre-load gralloc mapper — GraphicBufferMapper may fail");
}

void HybrisComposerVdiImpl::InitHwc2Device()
{
    PreloadGrallocMapper();

    eventListener_.on_hotplug_received = OnHotplug;
    eventListener_.on_vsync_received   = OnVsync;
    eventListener_.on_refresh_received = OnRefresh;

    device_ = hwc2_compat_device_new(false);
    if (!device_) {
        DISPLAY_LOGE("hwc2_compat_device_new failed — is the Android composer service running?");
        return;
    }

    /*
     * NOTE: hwc2_compat_device_register_callback is intentionally NOT called
     * here. The Android HWC2 HIDL composer fires the hotplug callback
     * synchronously inside registerCallback. If registerCallback were called
     * from the constructor, OnHotplug→GetVdiInstance() would re-enter the
     * C++ static-local guard while it is still locked, causing:
     *   __cxa_guard_acquire detected recursive initialization → SIGABRT
     *
     * Instead, registerCallback is deferred to the first RegHotPlugCallback()
     * call, by which time the static singleton is fully constructed and
     * GetVdiInstance() is safe to call from any callback.
     */

    DISPLAY_LOGI("HWC2 device created (callback registration deferred to RegHotPlugCallback)");
}

/* ── HWC2 event callbacks (static) ──────────────────────────────────────── */

void HybrisComposerVdiImpl::OnHotplug(HWC2EventListener* /*self*/, int32_t /*seq*/,
    hwc2_display_t display, bool connected, bool /*primary*/)
{
    GetVdiInstance().HandleHotplug(display, connected);
}

void HybrisComposerVdiImpl::OnVsync(HWC2EventListener* /*self*/, int32_t /*seq*/,
    hwc2_display_t display, int64_t timestamp)
{
    auto& inst = GetVdiInstance();
    std::lock_guard<std::mutex> lock(inst.mutex_);
    for (auto& kv : inst.displays_) {
        /* Match by hwc2 display id stored in HybrisDisplay */
        if (kv.second->GetHwc2Display() ==
            hwc2_compat_device_get_display_by_id(inst.device_, display)) {
            kv.second->OnVsync(timestamp);
            break;
        }
    }
}

void HybrisComposerVdiImpl::OnRefresh(HWC2EventListener* /*self*/, int32_t /*seq*/,
    hwc2_display_t /*display*/)
{
    /* Refresh requests can be ignored in this implementation */
}

void HybrisComposerVdiImpl::HandleHotplug(hwc2_display_t hwc2Id, bool connected)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (connected) {
        hwc2_compat_display_t* hwc2Disp =
            hwc2_compat_device_get_display_by_id(device_, hwc2Id);
        if (!hwc2Disp) {
            DISPLAY_LOGE("hwc2_compat_device_get_display_by_id(%llu) returned null",
                (unsigned long long)hwc2Id);
            return;
        }

        uint32_t devId = nextDevId_++;
        displays_[devId] = std::make_unique<HybrisDisplay>(devId, hwc2Disp);

        DISPLAY_LOGI("Display %u connected (hwc2 id=%llu)", devId,
            (unsigned long long)hwc2Id);

        if (hotplugCb_) {
            hotplugCb_(devId, true, hotplugData_);
        }
    } else {
        /* Find the devId that corresponds to this hwc2 display */
        for (auto it = displays_.begin(); it != displays_.end(); ++it) {
            hwc2_compat_display_t* candidate =
                hwc2_compat_device_get_display_by_id(device_, hwc2Id);
            if (it->second->GetHwc2Display() == candidate) {
                uint32_t devId = it->first;
                DISPLAY_LOGI("Display %u disconnected", devId);
                if (hotplugCb_) {
                    hotplugCb_(devId, false, hotplugData_);
                }
                displays_.erase(it);
                break;
            }
        }
    }
}

/* ── Lookup helpers ──────────────────────────────────────────────────────── */

HybrisDisplay* HybrisComposerVdiImpl::GetDisplay(uint32_t devId)
{
    auto it = displays_.find(devId);
    if (it == displays_.end()) {
        DISPLAY_LOGE("Display %u not found", devId);
        return nullptr;
    }
    return it->second.get();
}

HybrisLayer* HybrisComposerVdiImpl::GetLayer(uint32_t devId, uint32_t layerId)
{
    HybrisDisplay* disp = GetDisplay(devId);
    if (!disp) return nullptr;
    HybrisLayer* layer = disp->GetLayer(layerId);
    if (!layer) {
        DISPLAY_LOGE("Layer %u not found on display %u", layerId, devId);
    }
    return layer;
}

/* ── IDisplayComposerVdi implementation ──────────────────────────────────── */

int32_t HybrisComposerVdiImpl::RegHotPlugCallback(HotPlugCallback cb, void* data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    hotplugCb_ = cb;
    hotplugData_ = data;

    /*
     * Register the HWC2 callback now that the static singleton is fully
     * constructed. The Android composer fires hotplug synchronously inside
     * registerCallback, so OnHotplug→GetVdiInstance() is safe at this point.
     */
    if (!hwc2CallbackRegistered_ && device_) {
        hwc2CallbackRegistered_ = true;
        hwc2_compat_device_register_callback(device_, &eventListener_, composerSeq_++);
        /*
         * Trigger the hotplug callback for display 0 (primary) so that the
         * display is registered immediately. Some HWC2 implementations need
         * a manual nudge after registerCallback.
         */
        hwc2_compat_device_on_hotplug(device_, 0, true);
    }

    /* Re-fire for any already-connected displays */
    for (auto& kv : displays_) {
        cb(kv.first, true, data);
    }
    return HDF_SUCCESS;
}

int32_t HybrisComposerVdiImpl::GetDisplayCapability(uint32_t devId, DisplayCapability& info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->GetDisplayCapability(info);
}

int32_t HybrisComposerVdiImpl::GetDisplaySupportedModes(uint32_t devId,
    std::vector<DisplayModeInfo>& modes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->GetDisplaySupportedModes(modes);
}

int32_t HybrisComposerVdiImpl::GetDisplayMode(uint32_t devId, uint32_t& modeId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->GetDisplayMode(modeId);
}

int32_t HybrisComposerVdiImpl::SetDisplayMode(uint32_t devId, uint32_t modeId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->SetDisplayMode(modeId);
}

int32_t HybrisComposerVdiImpl::GetDisplayPowerStatus(uint32_t devId, DispPowerStatus& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->GetDisplayPowerStatus(status);
}

int32_t HybrisComposerVdiImpl::SetDisplayPowerStatus(uint32_t devId, DispPowerStatus status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->SetDisplayPowerStatus(status);
}

int32_t HybrisComposerVdiImpl::GetDisplayBacklight(uint32_t devId, uint32_t& level)
{
    DISPLAY_UNUSED(devId);
    level = 100;
    return HDF_SUCCESS;
}

int32_t HybrisComposerVdiImpl::SetDisplayBacklight(uint32_t devId, uint32_t level)
{
    DISPLAY_UNUSED(devId);
    DISPLAY_UNUSED(level);
    /* Backlight control not available via HWC2 compat API */
    return HDF_SUCCESS;
}

int32_t HybrisComposerVdiImpl::GetDisplayProperty(uint32_t devId, uint32_t id, uint64_t& value)
{
    DISPLAY_UNUSED(devId);
    DISPLAY_UNUSED(id);
    DISPLAY_UNUSED(value);
    return HDF_ERR_NOT_SUPPORT;
}

int32_t HybrisComposerVdiImpl::GetDisplayCompChange(uint32_t devId,
    std::vector<uint32_t>& layers, std::vector<int32_t>& types)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->GetDisplayCompChange(layers, types);
}

int32_t HybrisComposerVdiImpl::SetDisplayClientCrop(uint32_t devId, const IRect& rect)
{
    DISPLAY_UNUSED(devId);
    DISPLAY_UNUSED(rect);
    return HDF_ERR_NOT_SUPPORT;
}

int32_t HybrisComposerVdiImpl::SetDisplayClientBuffer(uint32_t devId,
    const BufferHandle& buffer, int32_t fence)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->SetDisplayClientBuffer(buffer, fence);
}

int32_t HybrisComposerVdiImpl::SetDisplayClientDamage(uint32_t devId, std::vector<IRect>& rects)
{
    DISPLAY_UNUSED(devId);
    DISPLAY_UNUSED(rects);
    return HDF_ERR_NOT_SUPPORT;
}

int32_t HybrisComposerVdiImpl::SetDisplayVsyncEnabled(uint32_t devId, bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->SetDisplayVsyncEnabled(enabled);
}

int32_t HybrisComposerVdiImpl::RegDisplayVBlankCallback(uint32_t devId, VBlankCallback cb,
    void* data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->RegDisplayVBlankCallback(cb, data);
}

int32_t HybrisComposerVdiImpl::GetDisplayReleaseFence(uint32_t devId,
    std::vector<uint32_t>& layers, std::vector<int32_t>& fences)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->GetDisplayReleaseFence(layers, fences);
}

int32_t HybrisComposerVdiImpl::CreateVirtualDisplay(uint32_t width, uint32_t height,
    int32_t& format, uint32_t& devId)
{
    DISPLAY_UNUSED(width);
    DISPLAY_UNUSED(height);
    DISPLAY_UNUSED(format);
    DISPLAY_UNUSED(devId);
    return HDF_ERR_NOT_SUPPORT;
}

int32_t HybrisComposerVdiImpl::DestroyVirtualDisplay(uint32_t devId)
{
    DISPLAY_UNUSED(devId);
    return HDF_ERR_NOT_SUPPORT;
}

int32_t HybrisComposerVdiImpl::SetVirtualDisplayBuffer(uint32_t devId,
    const BufferHandle& buffer, const int32_t fence)
{
    DISPLAY_UNUSED(devId);
    DISPLAY_UNUSED(buffer);
    DISPLAY_UNUSED(fence);
    return HDF_ERR_NOT_SUPPORT;
}

int32_t HybrisComposerVdiImpl::SetDisplayProperty(uint32_t devId, uint32_t id, uint64_t value)
{
    DISPLAY_UNUSED(devId);
    DISPLAY_UNUSED(id);
    DISPLAY_UNUSED(value);
    return HDF_ERR_NOT_SUPPORT;
}

int32_t HybrisComposerVdiImpl::Commit(uint32_t devId, int32_t& fence)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->Commit(fence);
}

int32_t HybrisComposerVdiImpl::CreateLayer(uint32_t devId, const LayerInfo& layerInfo,
    uint32_t& layerId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->CreateLayer(layerInfo, layerId);
}

int32_t HybrisComposerVdiImpl::DestroyLayer(uint32_t devId, uint32_t layerId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->DestroyLayer(layerId);
}

int32_t HybrisComposerVdiImpl::PrepareDisplayLayers(uint32_t devId, bool& needFlushFb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisDisplay* disp = GetDisplay(devId);
    DISPLAY_CHK_RETURN(disp == nullptr, HDF_FAILURE, );
    return disp->PrepareDisplayLayers(needFlushFb);
}

int32_t HybrisComposerVdiImpl::SetLayerAlpha(uint32_t devId, uint32_t layerId,
    const LayerAlpha& alpha)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerAlpha(alpha);
}

int32_t HybrisComposerVdiImpl::SetLayerRegion(uint32_t devId, uint32_t layerId, const IRect& rect)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerRegion(rect);
}

int32_t HybrisComposerVdiImpl::SetLayerCrop(uint32_t devId, uint32_t layerId, const IRect& rect)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerCrop(rect);
}

int32_t HybrisComposerVdiImpl::SetLayerZorder(uint32_t devId, uint32_t layerId, uint32_t zorder)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerZorder(zorder);
}

int32_t HybrisComposerVdiImpl::SetLayerPreMulti(uint32_t devId, uint32_t layerId, bool preMul)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerPreMulti(preMul);
}

int32_t HybrisComposerVdiImpl::SetLayerTransformMode(uint32_t devId, uint32_t layerId,
    TransformType type)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerTransformMode(type);
}

int32_t HybrisComposerVdiImpl::SetLayerDirtyRegion(uint32_t devId, uint32_t layerId,
    const std::vector<IRect>& rects)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerDirtyRegion(rects);
}

int32_t HybrisComposerVdiImpl::SetLayerVisibleRegion(uint32_t devId, uint32_t layerId,
    std::vector<IRect>& rects)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    /* Map visible region to hwc2 visible region (reuse dirty region path) */
    if (!rects.empty()) {
        const IRect& r = rects[0];
        hwc2_compat_layer_set_visible_region(layer->GetHwc2Layer(),
            r.x, r.y, r.x + r.w, r.y + r.h);
    }
    return HDF_SUCCESS;
}

int32_t HybrisComposerVdiImpl::SetLayerBuffer(uint32_t devId, uint32_t layerId,
    const BufferHandle& buffer, int32_t fence)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerBuffer(buffer, fence);
}

int32_t HybrisComposerVdiImpl::SetLayerCompositionType(uint32_t devId, uint32_t layerId,
    CompositionType type)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerCompositionType(type);
}

int32_t HybrisComposerVdiImpl::SetLayerBlendType(uint32_t devId, uint32_t layerId, BlendType type)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerBlendType(type);
}

int32_t HybrisComposerVdiImpl::SetLayerMaskInfo(uint32_t devId, uint32_t layerId,
    const MaskInfo maskInfo)
{
    DISPLAY_UNUSED(devId);
    DISPLAY_UNUSED(layerId);
    DISPLAY_UNUSED(maskInfo);
    return HDF_ERR_NOT_SUPPORT;
}

int32_t HybrisComposerVdiImpl::SetLayerColor(uint32_t devId, uint32_t layerId,
    const LayerColor& layerColor)
{
    std::lock_guard<std::mutex> lock(mutex_);
    HybrisLayer* layer = GetLayer(devId, layerId);
    DISPLAY_CHK_RETURN(layer == nullptr, HDF_FAILURE, );
    return layer->SetLayerColor(layerColor);
}

/* ── Factory functions (C ABI) ───────────────────────────────────────────── */

extern "C" IDisplayComposerVdi* CreateComposerVdi()
{
    return new HybrisComposerVdiImpl();
}

extern "C" void DestroyComposerVdi(IDisplayComposerVdi* vdi)
{
    delete vdi;
}

extern "C" int32_t GetDumpInfo(std::string& result)
{
    result = "HybrisComposerVdi: hwc2_compat backed display composer\n";
    return HDF_SUCCESS;
}

extern "C" int32_t UpdateConfig(std::string& /*result*/)
{
    return HDF_ERR_NOT_SUPPORT;
}

/* ── C wrapper shims (same pattern as x86_general) ───────────────────────── */

extern "C" int32_t RegHotPlugCallback(HotPlugCallback cb, void* data)
{
    return HybrisComposerVdiImpl::GetVdiInstance().RegHotPlugCallback(cb, data);
}
extern "C" int32_t GetDisplayCapability(uint32_t devId, V1_0::DisplayCapability& info)
{
    return HybrisComposerVdiImpl::GetVdiInstance().GetDisplayCapability(devId, info);
}
extern "C" int32_t GetDisplaySupportedModes(uint32_t devId, std::vector<V1_0::DisplayModeInfo>& modes)
{
    return HybrisComposerVdiImpl::GetVdiInstance().GetDisplaySupportedModes(devId, modes);
}
extern "C" int32_t GetDisplayMode(uint32_t devId, uint32_t& modeId)
{
    return HybrisComposerVdiImpl::GetVdiInstance().GetDisplayMode(devId, modeId);
}
extern "C" int32_t SetDisplayMode(uint32_t devId, uint32_t modeId)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetDisplayMode(devId, modeId);
}
extern "C" int32_t GetDisplayPowerStatus(uint32_t devId, V1_0::DispPowerStatus& status)
{
    return HybrisComposerVdiImpl::GetVdiInstance().GetDisplayPowerStatus(devId, status);
}
extern "C" int32_t SetDisplayPowerStatus(uint32_t devId, V1_0::DispPowerStatus status)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetDisplayPowerStatus(devId, status);
}
extern "C" int32_t GetDisplayBacklight(uint32_t devId, uint32_t& level)
{
    return HybrisComposerVdiImpl::GetVdiInstance().GetDisplayBacklight(devId, level);
}
extern "C" int32_t SetDisplayBacklight(uint32_t devId, uint32_t level)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetDisplayBacklight(devId, level);
}
extern "C" int32_t GetDisplayProperty(uint32_t devId, uint32_t id, uint64_t& value)
{
    return HybrisComposerVdiImpl::GetVdiInstance().GetDisplayProperty(devId, id, value);
}
extern "C" int32_t GetDisplayCompChange(uint32_t devId, std::vector<uint32_t>& layers,
    std::vector<int32_t>& types)
{
    return HybrisComposerVdiImpl::GetVdiInstance().GetDisplayCompChange(devId, layers, types);
}
extern "C" int32_t SetDisplayClientCrop(uint32_t devId, const V1_0::IRect& rect)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetDisplayClientCrop(devId, rect);
}
extern "C" int32_t SetDisplayClientBuffer(uint32_t devId, const BufferHandle& buffer, int32_t fence)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetDisplayClientBuffer(devId, buffer, fence);
}
extern "C" int32_t SetDisplayClientDamage(uint32_t devId, std::vector<V1_0::IRect>& rects)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetDisplayClientDamage(devId, rects);
}
extern "C" int32_t SetDisplayVsyncEnabled(uint32_t devId, bool enabled)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetDisplayVsyncEnabled(devId, enabled);
}
extern "C" int32_t RegDisplayVBlankCallback(uint32_t devId, VBlankCallback cb, void* data)
{
    return HybrisComposerVdiImpl::GetVdiInstance().RegDisplayVBlankCallback(devId, cb, data);
}
extern "C" int32_t GetDisplayReleaseFence(uint32_t devId, std::vector<uint32_t>& layers,
    std::vector<int32_t>& fences)
{
    return HybrisComposerVdiImpl::GetVdiInstance().GetDisplayReleaseFence(devId, layers, fences);
}
extern "C" int32_t CreateVirtualDisplay(uint32_t width, uint32_t height, int32_t& format,
    uint32_t& devId)
{
    return HybrisComposerVdiImpl::GetVdiInstance().CreateVirtualDisplay(width, height, format, devId);
}
extern "C" int32_t DestroyVirtualDisplay(uint32_t devId)
{
    return HybrisComposerVdiImpl::GetVdiInstance().DestroyVirtualDisplay(devId);
}
extern "C" int32_t SetVirtualDisplayBuffer(uint32_t devId, const BufferHandle& buffer,
    const int32_t fence)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetVirtualDisplayBuffer(devId, buffer, fence);
}
extern "C" int32_t SetDisplayProperty(uint32_t devId, uint32_t id, uint64_t value)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetDisplayProperty(devId, id, value);
}
extern "C" int32_t Commit(uint32_t devId, int32_t& fence)
{
    return HybrisComposerVdiImpl::GetVdiInstance().Commit(devId, fence);
}
extern "C" int32_t CreateLayer(uint32_t devId, const V1_0::LayerInfo& layerInfo, uint32_t& layerId)
{
    return HybrisComposerVdiImpl::GetVdiInstance().CreateLayer(devId, layerInfo, layerId);
}
extern "C" int32_t DestroyLayer(uint32_t devId, uint32_t layerId)
{
    return HybrisComposerVdiImpl::GetVdiInstance().DestroyLayer(devId, layerId);
}
extern "C" int32_t PrepareDisplayLayers(uint32_t devId, bool& needFlushFb)
{
    return HybrisComposerVdiImpl::GetVdiInstance().PrepareDisplayLayers(devId, needFlushFb);
}
extern "C" int32_t SetLayerAlpha(uint32_t devId, uint32_t layerId, const V1_0::LayerAlpha& alpha)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerAlpha(devId, layerId, alpha);
}
extern "C" int32_t SetLayerRegion(uint32_t devId, uint32_t layerId, const V1_0::IRect& rect)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerRegion(devId, layerId, rect);
}
extern "C" int32_t SetLayerCrop(uint32_t devId, uint32_t layerId, const V1_0::IRect& rect)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerCrop(devId, layerId, rect);
}
extern "C" int32_t SetLayerZorder(uint32_t devId, uint32_t layerId, uint32_t zorder)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerZorder(devId, layerId, zorder);
}
extern "C" int32_t SetLayerPreMulti(uint32_t devId, uint32_t layerId, bool preMul)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerPreMulti(devId, layerId, preMul);
}
extern "C" int32_t SetLayerTransformMode(uint32_t devId, uint32_t layerId, V1_0::TransformType type)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerTransformMode(devId, layerId, type);
}
extern "C" int32_t SetLayerDirtyRegion(uint32_t devId, uint32_t layerId,
    const std::vector<V1_0::IRect>& rects)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerDirtyRegion(devId, layerId, rects);
}
extern "C" int32_t SetLayerVisibleRegion(uint32_t devId, uint32_t layerId,
    std::vector<V1_0::IRect>& rects)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerVisibleRegion(devId, layerId, rects);
}
extern "C" int32_t SetLayerBuffer(uint32_t devId, uint32_t layerId, const BufferHandle& buffer,
    int32_t fence)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerBuffer(devId, layerId, buffer, fence);
}
extern "C" int32_t SetLayerCompositionType(uint32_t devId, uint32_t layerId,
    V1_0::CompositionType type)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerCompositionType(devId, layerId, type);
}
extern "C" int32_t SetLayerBlendType(uint32_t devId, uint32_t layerId, V1_0::BlendType type)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerBlendType(devId, layerId, type);
}
extern "C" int32_t SetLayerMaskInfo(uint32_t devId, uint32_t layerId, const V1_0::MaskInfo maskInfo)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerMaskInfo(devId, layerId, maskInfo);
}
extern "C" int32_t SetLayerColor(uint32_t devId, uint32_t layerId, const V1_0::LayerColor& color)
{
    return HybrisComposerVdiImpl::GetVdiInstance().SetLayerColor(devId, layerId, color);
}

} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS
