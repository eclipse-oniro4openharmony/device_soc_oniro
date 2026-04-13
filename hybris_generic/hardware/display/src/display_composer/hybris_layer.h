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

#ifndef HYBRIS_LAYER_H
#define HYBRIS_LAYER_H

#include <cstdint>
#include <memory>
#include "buffer_handle.h"
#include "v1_0/display_composer_type.h"
#include <hybris/hwc2/hwc2_compatibility_layer.h>

namespace OHOS {
namespace HDI {
namespace DISPLAY {

using namespace OHOS::HDI::Display::Composer::V1_0;

/*
 * HybrisLayer wraps a single hwc2_compat_layer_t and stores per-layer
 * state needed to drive the Android HWC2 HAL.
 */
class HybrisLayer {
public:
    explicit HybrisLayer(hwc2_compat_layer_t* layer, uint32_t id);
    ~HybrisLayer() = default;

    uint32_t GetId() const { return id_; }
    hwc2_compat_layer_t* GetHwc2Layer() const { return layer_; }

    int32_t SetLayerAlpha(const LayerAlpha& alpha);
    int32_t SetLayerRegion(const IRect& rect);
    int32_t SetLayerCrop(const IRect& rect);
    int32_t SetLayerZorder(uint32_t zorder);
    int32_t SetLayerPreMulti(bool preMul);
    int32_t SetLayerTransformMode(TransformType type);
    int32_t SetLayerDirtyRegion(const std::vector<IRect>& rects);
    int32_t SetLayerBuffer(const BufferHandle& buffer, int32_t fence);
    int32_t SetLayerCompositionType(CompositionType type);
    int32_t SetLayerBlendType(BlendType type);
    int32_t SetLayerColor(const LayerColor& color);

    CompositionType GetCompositionType() const { return compType_; }

private:
    hwc2_compat_layer_t* layer_{nullptr};
    uint32_t id_{0};

    /* Compose type reported back during GetDisplayCompChange */
    CompositionType compType_{COMPOSITION_CLIENT};

    static int32_t OhosToHwc2CompositionType(CompositionType type);
    static int32_t OhosToHwc2BlendMode(BlendType type);
    static int32_t OhosToHwc2Transform(TransformType type);
};

} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS

#endif // HYBRIS_LAYER_H
