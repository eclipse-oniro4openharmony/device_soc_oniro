/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
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

#include "display_buffer_vdi_impl.h"
#include "cinttypes"
#include "display_common.h"
#include "allocator.h"
#include "allocator_manager.h"
#include "hdf_base.h"
#include "v1_0/display_composer_type.h"

namespace OHOS {
namespace HDI {
namespace DISPLAY {
using namespace OHOS::HDI::Display::Composer::V1_0;
using namespace OHOS::HDI::Display::Buffer::V1_2;

DisplayBufferVdiImpl::DisplayBufferVdiImpl()
{
    int ret = AllocatorManager::GetInstance().Init();
    if (ret != HDF_SUCCESS) {
        DISPLAY_LOGE("gbm construct failed");
    }
}

DisplayBufferVdiImpl::~DisplayBufferVdiImpl()
{
    AllocatorManager::GetInstance().DeInit();
}

int32_t DisplayBufferVdiImpl::AllocMem(const AllocInfo& info, BufferHandle*& handle) const
{
    return AllocatorManager::GetInstance().GetAllocator(info.usage)->AllocMem(info, &handle);
}

void DisplayBufferVdiImpl::FreeMem(const BufferHandle& handle) const
{
    AllocatorManager::GetInstance().GetAllocator(handle.usage)->FreeMem(const_cast<BufferHandle *>(&handle));
}

void* DisplayBufferVdiImpl::Mmap(const BufferHandle& handle) const
{
    return AllocatorManager::GetInstance().GetAllocator(handle.usage)->Mmap(const_cast<BufferHandle *>(&handle));
}

int32_t DisplayBufferVdiImpl::Unmap(const BufferHandle& handle) const
{
    return AllocatorManager::GetInstance().GetAllocator(handle.usage)->Unmap(const_cast<BufferHandle *>(&handle));
}

int32_t DisplayBufferVdiImpl::FlushCache(const BufferHandle& handle) const
{
    return AllocatorManager::GetInstance().GetAllocator(handle.usage)->FlushCache(const_cast<BufferHandle *>(&handle));
}

int32_t DisplayBufferVdiImpl::InvalidateCache(const BufferHandle& handle) const
{
    return AllocatorManager::GetInstance().GetAllocator(handle.usage)->InvalidateCache(const_cast<BufferHandle *>(&handle));
}

int32_t DisplayBufferVdiImpl::IsSupportedAlloc(const std::vector<VerifyAllocInfo>& infos,
    std::vector<bool>& supporteds) const
{
    return DISPLAY_NOT_SUPPORT;
}

int32_t DisplayBufferVdiImpl::RegisterBuffer(const BufferHandle& handle)
{
    DISPLAY_LOGE("%s is not supported", __func__);
    return DISPLAY_NOT_SUPPORT;
}

int32_t DisplayBufferVdiImpl::SetMetadata(const BufferHandle& handle, uint32_t key, const std::vector<uint8_t>& value)
{
    DISPLAY_LOGE("%s is not supported", __func__);
    return DISPLAY_NOT_SUPPORT;
}

int32_t DisplayBufferVdiImpl::GetMetadata(const BufferHandle& handle, uint32_t key, std::vector<uint8_t>& value)
{
    DISPLAY_LOGE("%s is not supported", __func__);
    return DISPLAY_NOT_SUPPORT;
}

int32_t DisplayBufferVdiImpl::ListMetadataKeys(const BufferHandle& handle, std::vector<uint32_t>& keys)
{
    DISPLAY_LOGE("%s is not supported", __func__);
    return DISPLAY_NOT_SUPPORT;
}

int32_t DisplayBufferVdiImpl::EraseMetadataKey(const BufferHandle& handle, uint32_t key)
{
    DISPLAY_LOGE("%s is not supported", __func__);
    return DISPLAY_NOT_SUPPORT;
}

int32_t DisplayBufferVdiImpl::GetImageLayout(const BufferHandle& handle, ImageLayout& layout) const
{
    DISPLAY_LOGE("%s is not supported", __func__);
    return DISPLAY_NOT_SUPPORT;
}

extern "C" IDisplayBufferVdi* CreateDisplayBufferVdi()
{
    return new DisplayBufferVdiImpl();
}

extern "C" void DestroyDisplayBufferVdi(IDisplayBufferVdi* vdi)
{
    delete vdi;
}
} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS
