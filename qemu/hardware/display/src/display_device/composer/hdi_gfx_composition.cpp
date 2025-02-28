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

#include "hdi_gfx_composition.h"
#include <cinttypes>
#include <dlfcn.h>
#include <cerrno>
#include "display_common.h"
#include "display_gfx.h"
#include "hitrace_meter.h"
#include "v1_0/display_composer_type.h"

using namespace OHOS::HDI::Display::Composer::V1_0;

namespace OHOS {
namespace HDI {
namespace DISPLAY {
int32_t HdiGfxComposition::Init(void)
{
    DISPLAY_LOGD();
    return DISPLAY_SUCCESS;
}

int32_t HdiGfxComposition::SetLayers(std::vector<HdiLayer *> &layers, HdiLayer &clientLayer)
{
    DISPLAY_LOGD("layers size %{public}zd", layers.size());
    mClientLayer = &clientLayer;
    mCompLayers.clear();
    for (auto &layer : layers) {
        layer->SetDeviceSelect(COMPOSITION_CLIENT);
    }
    DISPLAY_LOGD("composer layers size %{public}zd", mCompLayers.size());
    return DISPLAY_SUCCESS;
}

int32_t HdiGfxComposition::Apply(bool modeSet)
{
    DISPLAY_LOGD("[HdiGfxComposition::Apply] composer layers size %{public}zd", mCompLayers.size());
    return DISPLAY_SUCCESS;
}
} // namespace OHOS
} // namespace HDI
} // namespace DISPLAY
