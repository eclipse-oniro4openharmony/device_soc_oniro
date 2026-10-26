/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
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

#include "drm_display.h"
#include <string>
#include <cerrno>
#include <memory>
#include <vector>
#include <sys/ioctl.h>
#include <unistd.h>
#include <securec.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "display_buffer_vdi_impl.h"
#include "display_common.h"
#include "drm_device.h"
#include "drm_vsync_worker.h"
#include "hdi_drm_composition.h"
#include "hdi_gfx_composition.h"
#include "idisplay_buffer_vdi.h"

namespace OHOS {
namespace HDI {
namespace DISPLAY {
using namespace OHOS::HDI::Display::Buffer::V1_0;
DrmDisplay::DrmDisplay(std::shared_ptr<DrmConnector> connector, std::shared_ptr<DrmCrtc> crtc,
    std::shared_ptr<DrmDevice> drmDevice)
    : mDrmDevice(drmDevice), mConnector(connector), mCrtc(crtc)
{}

DrmDisplay::~DrmDisplay()
{
    if (mCrtc != nullptr) {
        mCrtc->UnBindDisplay(GetId());
    }
    mBlackLayer.reset();
    if (mBlackBuffer != nullptr) {
        DisplayBufferVdiImpl().FreeMem(*mBlackBuffer);
    }
}

int32_t DrmDisplay::Init()
{
    int ret;
    DISPLAY_CHK_RETURN((mCrtc == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("crtc is null"));
    DISPLAY_CHK_RETURN((mConnector == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("connector is null"));
    DISPLAY_CHK_RETURN((mDrmDevice == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("drmDevice is null"));

    ret = HdiDisplay::Init();
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("init failed"));
    auto preComp = std::make_unique<HdiGfxComposition>();
    DISPLAY_CHK_RETURN((preComp == nullptr), DISPLAY_FAILURE,
        DISPLAY_LOGE("can not new HdiGfxComposition errno %{public}d", errno));
    ret = preComp->Init();
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("can not init HdiGfxComposition"));

    auto postComp = std::make_unique<HdiDrmComposition>(mConnector, mCrtc, mDrmDevice);
    DISPLAY_CHK_RETURN((postComp == nullptr), DISPLAY_FAILURE,
        DISPLAY_LOGE("can not new HdiDrmComposition errno %{public}d", errno));
    ret = postComp->Init();
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("can not init HdiDrmComposition"));
    mComposer = std::make_unique<HdiComposer>(std::move(preComp), std::move(postComp));
    ret = mCrtc->BindToDisplay(GetId());
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("bind crtc failed"));

    ret = ChosePreferenceMode();
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("choose preference mode fialed"));
    return DISPLAY_SUCCESS;
}

int32_t DrmDisplay::GetDisplayCapability(DisplayCapability *info)
{
    mConnector->GetDisplayCap(*info);
    return DISPLAY_SUCCESS;
}

int32_t DrmDisplay::GetDisplaySupportedModes(uint32_t *num, DisplayModeInfo *modes)
{
    mConnector->GetDisplaySupportedModes(num, modes);
    return DISPLAY_SUCCESS;
}

int32_t DrmDisplay::GetDisplayMode(uint32_t *modeId)
{
    DISPLAY_CHK_RETURN((modeId == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("the in modeId is nullptr"));
    *modeId = mCrtc->GetActiveModeId();
    return DISPLAY_SUCCESS;
}

int32_t DrmDisplay::SetDisplayMode(uint32_t modeId)
{
    return mCrtc->SetActivieMode(modeId);
}

int32_t DrmDisplay::GetDisplayPowerStatus(DispPowerStatus *status)
{
    DISPLAY_CHK_RETURN((status == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("status is nullptr"));
    std::lock_guard<std::mutex> lock(mPowerMutex);
    *status = mPowerStatus;
    return DISPLAY_SUCCESS;
}

/*
 * The emulator's virtio-gpu output has no panel to switch off. With the scanout
 * disabled, QEMU draws its own placeholder; with it enabled, the window keeps the
 * last frame, which looks like a live screen. So a power-off shows an all-black
 * frame, and RenderService's frames are dropped until the power is back on.
 */
int32_t DrmDisplay::SetDisplayPowerStatus(DispPowerStatus status)
{
    DISPLAY_LOGD("the status %{public}d ", status);
    uint32_t drmPowerState = 0;
    int ret = ConvertToDrmPowerState(status, drmPowerState);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_PARAM_ERR,
        DISPLAY_LOGE("unknown power status %{public}d", status));
    std::lock_guard<std::mutex> lock(mPowerMutex);
    if (status != POWER_STATUS_ON && mPowerStatus == POWER_STATUS_ON && ShowBlackFrame() != DISPLAY_SUCCESS) {
        DISPLAY_LOGE("can not blank the screen");
    }
    mPowerStatus = status;
    return DISPLAY_SUCCESS;
}

int32_t DrmDisplay::Commit(int32_t *fence)
{
    DISPLAY_CHK_RETURN((fence == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("fence is nullptr"));
    std::lock_guard<std::mutex> lock(mPowerMutex);
    if (mPowerStatus != POWER_STATUS_ON) {
        *fence = -1;
        return DISPLAY_SUCCESS;
    }
    return HdiDisplay::Commit(fence);
}

int32_t DrmDisplay::ShowBlackFrame()
{
    if (mBlackLayer == nullptr) {
        DrmMode mode;
        int ret = mConnector->GetModeFromId(mCrtc->GetActiveModeId(), mode);
        DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE,
            DISPLAY_LOGE("can not get the mode from id %{public}d", mCrtc->GetActiveModeId()));
        // virtio-gpu planes only scan out XRGB8888.
        const AllocInfo info = {
            .width = mode.GetModeInfoPtr()->hdisplay,
            .height = mode.GetModeInfoPtr()->vdisplay,
            .usage = HBM_USE_MEM_DMA | HBM_USE_CPU_READ | HBM_USE_CPU_WRITE,
            .format = PIXEL_FMT_BGRX_8888
        };
        DisplayBufferVdiImpl bufferImpl;
        BufferHandle *buffer = nullptr;
        ret = bufferImpl.AllocMem(info, buffer);
        DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("can not alloc memory"));
        void *addr = bufferImpl.Mmap(*buffer);
        if (addr == nullptr) {
            DISPLAY_LOGE("can not map the black buffer");
            bufferImpl.FreeMem(*buffer);
            return DISPLAY_FAILURE;
        }
        (void)memset_s(addr, buffer->size, 0, buffer->size);
        bufferImpl.Unmap(*buffer);
        mBlackBuffer = buffer;
        mBlackLayer = CreateHdiLayer(LAYER_TYPE_GRAPHIC);
        mBlackLayer->SetLayerBuffer(mBlackBuffer, -1);
    }

    std::vector<HdiLayer *> layers;
    HdiDrmComposition *drmComp = static_cast<HdiDrmComposition *>(mComposer->GetPostCompostion(0));
    drmComp->SetLayers(layers, *mBlackLayer);
    int32_t ret = drmComp->Apply(false);
    drmComp->SetLayers(layers, *mClientLayer);
    for (int fd = mBlackLayer->GetReleaseFenceFd(); fd >= 0; fd = mBlackLayer->GetReleaseFenceFd()) {
        close(fd);
    }
    return ret;
}

int32_t DrmDisplay::ConvertToDrmPowerState(DispPowerStatus hdiPowerState, uint32_t &drmPowerState)
{
    int32_t ret = DISPLAY_SUCCESS;
    switch (hdiPowerState) {
        case POWER_STATUS_OFF:
            drmPowerState = DRM_MODE_DPMS_OFF;
            break;
        case POWER_STATUS_ON:
            drmPowerState = DRM_MODE_DPMS_ON;
            break;
        case POWER_STATUS_STANDBY:
            drmPowerState = DRM_MODE_DPMS_STANDBY;
            break;
        case POWER_STATUS_SUSPEND:
            drmPowerState = DRM_MODE_DPMS_SUSPEND;
            break;
        default:
            ret = DISPLAY_FAILURE;
            break;
    }
    return ret;
}

std::unique_ptr<HdiLayer> DrmDisplay::CreateHdiLayer(LayerType type)
{
    DISPLAY_LOGD();
    return std::make_unique<HdiDrmLayer>(type);
}

int32_t DrmDisplay::WaitForVBlank(uint64_t *ns)
{
    int ret;
    constexpr uint64_t nPerS = 1000000000;
    constexpr uint64_t nPerUS = 1000;
    drmVBlank vbl = {
        .request.type = DRM_VBLANK_RELATIVE,
        .request.sequence = 0,
        .request.signal = 0,
    };
    DISPLAY_CHK_RETURN((ns == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("in ns is nullptr"));
    ret = drmWaitVBlank(mDrmDevice->GetDrmFd(), &vbl);
    DISPLAY_CHK_RETURN((ret != 0), DISPLAY_FAILURE, DISPLAY_LOGE("wait vblank failed errno %{public}d", errno));
    *ns = static_cast<uint64_t>(vbl.reply.tval_sec * nPerS + vbl.reply.tval_usec * nPerUS);
    return DISPLAY_SUCCESS;
}

bool DrmDisplay::IsConnected()
{
    DISPLAY_LOGD("conneted %{public}d", mConnector->IsConnected());
    return mConnector->IsConnected();
}

int32_t DrmDisplay::PushFirstFrame()
{
    std::shared_ptr<IDisplayBufferVdi> hdiImpl = std::make_shared<DisplayBufferVdiImpl>();
    int ret = DISPLAY_SUCCESS;
    DrmMode mode;
    ret = mConnector->GetModeFromId(mCrtc->GetActiveModeId(), mode);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE,
        DISPLAY_LOGE("can not get the mode from id %{public}d", mCrtc->GetActiveModeId()));
    const AllocInfo info = {
        .width = mode.GetModeInfoPtr()->hdisplay,
        .height = mode.GetModeInfoPtr()->vdisplay,
        .usage = HBM_USE_MEM_DMA | HBM_USE_CPU_READ | HBM_USE_CPU_WRITE,
        .format = PIXEL_FMT_BGRA_8888
    };

    BufferHandle *buffer = nullptr;
    ret = hdiImpl->AllocMem(info, buffer);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("can not alloc memory"));
    mClientLayer->SetLayerBuffer(buffer, -1);

    std::vector<HdiLayer *> layers;
    HdiDrmComposition *drmComp = static_cast<HdiDrmComposition *>(mComposer->GetPostCompostion(0));
    drmComp->SetLayers(layers, *mClientLayer);
    drmComp->Apply(true);
    return DISPLAY_SUCCESS;
}

int32_t DrmDisplay::ChosePreferenceMode()
{
    int32_t ret;
    int32_t modeId = mConnector->GetPreferenceId();
    if (modeId == INVALID_MODE_ID) {
        uint32_t num = 0;
        ret = GetDisplaySupportedModes(&num, nullptr);
        DISPLAY_CHK_RETURN((num == 0) && (ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("can not get modes"));
        modeId = 0;
    }
    ret = SetDisplayMode(modeId);
    // Push first frame to the drm, for that the vblank must init all the components.
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("set display mode failed"));
    return PushFirstFrame();
}

int32_t DrmDisplay::RegDisplayVBlankCallback(VBlankCallback cb, void *data)
{
    std::shared_ptr<VsyncCallBack> vsyncCb = std::make_shared<VsyncCallBack>(cb, data);
    DrmVsyncWorker::GetInstance().ReqesterVBlankCb(vsyncCb);
    return DISPLAY_SUCCESS;
}

int32_t DrmDisplay::SetDisplayVsyncEnabled(bool enabled)
{
    DISPLAY_LOGD("enable %{public}d", enabled);
    DrmVsyncWorker::GetInstance().EnableVsync(enabled);
    return DISPLAY_SUCCESS;
}

int32_t DrmDisplay::GetDisplayBacklight(uint32_t *value)
{
    DISPLAY_CHK_RETURN((value == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("value is nullptr"));
    return mConnector->GetBrightness(*value);
}

int32_t DrmDisplay::SetDisplayBacklight(uint32_t value)
{
    return mConnector->SetBrightness(value);
}
} // namespace OHOS
} // namespace HDI
} // namespace DISPLAY
