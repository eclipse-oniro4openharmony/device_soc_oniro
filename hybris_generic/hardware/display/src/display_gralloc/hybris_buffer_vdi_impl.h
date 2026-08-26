/*
 * Copyright (c) 2024 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
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

#ifndef HYBRIS_BUFFER_VDI_IMPL_H
#define HYBRIS_BUFFER_VDI_IMPL_H

#include "buffer_handle.h"
#include "idisplay_buffer_vdi.h"
#include "v1_0/display_buffer_type.h"

namespace OHOS {
namespace HDI {
namespace DISPLAY {

using namespace OHOS::HDI::Display::Buffer::V1_0;

/*
 * HybrisBufferVdiImpl — implements IDisplayBufferVdi by wrapping libhybris
 * gralloc (which talks to the Android gralloc HAL via hw_get_module / HIDL).
 *
 * Buffer ownership model:
 *   - AllocMem: calls hybris_gralloc_allocate, wraps the returned
 *     buffer_handle_t into an OHOS BufferHandle.  The native handle pointer is
 *     stored in the last two int32_t slots of BufferHandle::reserve[] so it
 *     can be recovered for gralloc calls without reconstructing it.
 *   - FreeMem: recovers the native handle, calls hybris_gralloc_release, then
 *     free()s the BufferHandle allocation.
 *   - Mmap / Unmap: lock / unlock via hybris_gralloc_lock / unlock.
 *   - FlushCache / InvalidateCache: implicit via unlock / re-lock; return
 *     DISPLAY_SUCCESS as the gralloc HAL handles coherency internally.
 */
class HybrisBufferVdiImpl : public IDisplayBufferVdi {
public:
    HybrisBufferVdiImpl();
    ~HybrisBufferVdiImpl() override = default;

    int32_t AllocMem(const AllocInfo& info, BufferHandle*& handle) const override;
    void FreeMem(const BufferHandle& handle) const override;
    void* Mmap(const BufferHandle& handle) const override;
    int32_t Unmap(const BufferHandle& handle) const override;
    int32_t FlushCache(const BufferHandle& handle) const override;
    int32_t InvalidateCache(const BufferHandle& handle) const override;
    int32_t IsSupportedAlloc(
        const std::vector<VerifyAllocInfo>& infos,
        std::vector<bool>& supporteds) const override;

    /*
     * Buffer metadata — not implemented on this port.
     *
     * These must be overridden: the IDisplayBufferVdi defaults return 0
     * (DISPLAY_SUCCESS) while storing nothing, so GetMetadata reports success
     * and hands back an empty value.  Callers that trust that contract (and
     * the HATS DisplayBufferUt metadata round-trip) then see a silent data
     * loss instead of an honest "unsupported".
     *
     * DISPLAY_NOT_SUPPORT matches the reference VDI
     * (drivers/peripheral/display/buffer/vdi_base/src/display_buffer_vdi_impl.cpp),
     * and SurfaceBufferImpl already treats it as a non-error for
     * RegisterBuffer (surface_buffer_impl.cpp:253, :594).
     */
    int32_t RegisterBuffer(const BufferHandle& handle) override;
    int32_t SetMetadata(const BufferHandle& handle, uint32_t key,
                        const std::vector<uint8_t>& value) override;
    int32_t GetMetadata(const BufferHandle& handle, uint32_t key,
                        std::vector<uint8_t>& value) override;
    int32_t ListMetadataKeys(const BufferHandle& handle, std::vector<uint32_t>& keys) override;
    int32_t EraseMetadataKey(const BufferHandle& handle, uint32_t key) override;
};

} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS

#endif // HYBRIS_BUFFER_VDI_IMPL_H
