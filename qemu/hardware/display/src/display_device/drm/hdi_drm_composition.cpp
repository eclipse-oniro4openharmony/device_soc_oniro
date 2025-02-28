/*
 * Copyright (c) 2021-2023 Huawei Device Co., Ltd.
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

#include "hdi_drm_composition.h"
#include <cerrno>
#include "hdi_drm_layer.h"

namespace OHOS {
namespace HDI {
namespace DISPLAY {
HdiDrmComposition::HdiDrmComposition(std::shared_ptr<DrmConnector> &connector, const std::shared_ptr<DrmCrtc> &crtc,
    std::shared_ptr<DrmDevice> &drmDevice)
        : mDrmDevice(drmDevice), mConnector(connector), mCrtc(crtc)
    {
        DISPLAY_LOGD();
    }

    // Initialize the DRM composition
    int32_t HdiDrmComposition::Init()
    {
        DISPLAY_LOGD();
        mPrimPlanes.clear();
        mOverlayPlanes.clear();
        mPlanes.clear();
        DISPLAY_CHK_RETURN((mCrtc == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("crtc is null"));
        DISPLAY_CHK_RETURN((mConnector == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("connector is null"));
        DISPLAY_CHK_RETURN((mDrmDevice == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("drmDevice is null"));
        mPrimPlanes = mDrmDevice->GetDrmPlane(mCrtc->GetPipe(), DRM_PLANE_TYPE_PRIMARY);
        mOverlayPlanes = mDrmDevice->GetDrmPlane(mCrtc->GetPipe(), DRM_PLANE_TYPE_OVERLAY);
        DISPLAY_CHK_RETURN((mPrimPlanes.size() == 0), DISPLAY_FAILURE, DISPLAY_LOGE("has no primary plane"));
        mPlanes.insert(mPlanes.end(), mPrimPlanes.begin(), mPrimPlanes.end());
        mPlanes.insert(mPlanes.end(), mOverlayPlanes.begin(), mOverlayPlanes.end());
        return DISPLAY_SUCCESS;
    }

    // Set the layers for composition
    int32_t HdiDrmComposition::SetLayers(std::vector<HdiLayer *> &layers, HdiLayer &clientLayer)
    {
        // now we do not surpport present direct
        DISPLAY_LOGD();
        mCompLayers.clear();
        mCompLayers.push_back(&clientLayer);
        return DISPLAY_SUCCESS;
    }

    // Apply the composition settings
    int32_t HdiDrmComposition::Apply(bool modeSet)
    {
        DISPLAY_LOGD();
        if (mCompLayers.empty()) {
            DISPLAY_LOGE("No layers to apply");
            return DISPLAY_FAILURE;
        }

        DISPLAY_LOGD("Number of layers to apply: %{public}zu", mCompLayers.size());

        HdiDrmLayer *layer = static_cast<HdiDrmLayer *>(mCompLayers[0]);
        DrmGemBuffer *buffer = layer->GetGemBuffer();
        if (!buffer || !buffer->IsValid()) {
            DISPLAY_LOGE("Invalid buffer");
            return DISPLAY_FAILURE;
        }

        int fd = DrmDevice::GetDrmFd();
        DrmMode mode;
        int ret = mConnector->GetModeFromId(mCrtc->GetActiveModeId(), mode);
        DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE,
            DISPLAY_LOGE("Cannot get the mode from id %{public}d", mCrtc->GetActiveModeId()));

        drmModeEncoder *enc = drmModeGetEncoder(fd, mConnector->GetEncoderId());
        if (!enc) {
            DISPLAY_LOGE("No encoder found");
            return DISPLAY_FAILURE;
        }

        uint32_t fb = buffer->GetFbId();

        uint32_t connector_id = mConnector->GetId();
        ret = drmModeSetCrtc(fd, enc->crtc_id, fb, 0, 0, &connector_id, 1, mode.GetModeInfoPtr());
        drmModeFreeEncoder(enc);

        if (ret != 0) {
            DISPLAY_LOGE("Failed to set CRTC: %{public}s", strerror(errno));
            return DISPLAY_FAILURE;
        }

        return DISPLAY_SUCCESS;
    }

} // OHOS
} // HDI
} // DISPLAY
