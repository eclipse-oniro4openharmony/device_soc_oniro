/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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
#include "allocator_manager.h"
#include "display_common.h"
#include "drm_allocator.h"

namespace OHOS {
namespace HDI {
namespace DISPLAY {
int32_t AllocatorManager::Init()
{
    DISPLAY_LOGD("AllocatorManager::Init");
    std::lock_guard<std::mutex> lock(mutex_);
    if (users_ > 0) {
        users_++;
        DISPLAY_LOGW("allocator has initialized");
        return DISPLAY_SUCCESS;
    }
    // first use drm allocator
    std::shared_ptr<Allocator> drmAllocator = std::make_shared<DrmAllocator>();
    int ret = drmAllocator->Init();
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE,
            DISPLAY_LOGE("Failed init drm allocator"));

    frameBufferAllocator_ = drmAllocator;
    allocator_ = drmAllocator;

    users_ = 1;
    DISPLAY_LOGD("init success");
    return DISPLAY_SUCCESS;
}

// Every DisplayBufferVdiImpl, short-lived ones included, inits and deinits the
// shared allocators: release them only when the last one goes.
int32_t AllocatorManager::DeInit()
{
    DISPLAY_LOGD();
    std::lock_guard<std::mutex> lock(mutex_);
    if (users_ == 0 || --users_ > 0) {
        return DISPLAY_SUCCESS;
    }
    frameBufferAllocator_.reset();
    allocator_.reset();
    return DISPLAY_SUCCESS;
}

Allocator* AllocatorManager::GetAllocator(uint64_t usage)
{
    DISPLAY_LOGD();
    if ((usage & HBM_USE_MEM_FB) != 0) {
        return frameBufferAllocator_.get();
    } else {
        return allocator_.get();
    }
}
}  // namespace DISPLAY
}  // namespace HDI
}  // namespace OHOS