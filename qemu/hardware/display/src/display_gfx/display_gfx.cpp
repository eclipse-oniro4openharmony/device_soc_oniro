/*
 * Copyright (c) 2024 Diemit <598757652@qq.com>
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

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include "display_common.h"
#include "securec.h"
#include "display_gfx.h"
#include "v1_0/display_composer_type.h"
namespace OHOS {
namespace HDI {
namespace DISPLAY {
using namespace OHOS::HDI::Display::Composer::V1_0;

int32_t GfxInitialize(GfxFuncs **funcs)
{
    DISPLAY_CHK_RETURN((funcs == NULL), DISPLAY_PARAM_ERR, DISPLAY_LOGE("info is null"));
    *funcs = NULL;
    return DISPLAY_SUCCESS;
}

int32_t GfxUninitialize(GfxFuncs *funcs)
{
    CHECK_NULLPOINTER_RETURN_VALUE(funcs, DISPLAY_NULL_PTR);
    free(funcs);
    DISPLAY_LOGI("%{public}s: gfx uninitialize success", __func__);
    return DISPLAY_SUCCESS;
}
} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS