/*
 * Copyright (c) 2026 Oniro Authors
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

#ifndef HYBRIS_BUFFER_LAYOUT_H
#define HYBRIS_BUFFER_LAYOUT_H

#include <cstdint>

namespace OHOS {
namespace HDI {
namespace DISPLAY {

/*
 * The buffer VDI mirrors an Android native_handle_t into an OHOS BufferHandle
 * and appends kPtrSlots bookkeeping slots at the end of reserve[]:
 *
 *   [0 .. reserveFds-1]                    — extra fds
 *   [reserveFds .. +numInts-1]             — the native_handle ints
 *   [.. +kPtrSlots-1]                      — bookkeeping, NOT part of the handle:
 *        [0], [1] — the 64-bit buffer_handle_t pointer (lo, hi halves)
 *        [2]      — pid of the process that pointer belongs to
 *        [3]      — how that process obtained it (HandleOwner)
 *
 * Anyone rebuilding a native_handle_t from a BufferHandle must exclude these
 * trailing slots, otherwise the HAL receives a handle with junk ints appended.
 */
static constexpr uint32_t kPtrSlots = 4;

/*
 * The stored pointer is only valid inside the process that produced it, and a
 * BufferHandle routinely crosses an IPC boundary: AllocMem runs in
 * allocator_host while Mmap/Unmap/FreeMem run locally in every client via the
 * passthrough mapper.  Android 14's Gralloc5 mapper dereferences the handle
 * immediately in handle_cast<imported_handle>(), so passing it a pointer from
 * another process is an instant SIGSEGV — Gralloc0/1 merely returned an error,
 * which is why this went unnoticed on Halium 12 devices.
 */
enum HandleOwner : int32_t {
    HANDLE_OWNER_NONE      = 0,
    HANDLE_OWNER_ALLOCATED = 1, /* allocated here — FreeMem must free the allocation */
    HANDLE_OWNER_IMPORTED  = 2, /* imported here — only the import reference is ours */
};

} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS

#endif // HYBRIS_BUFFER_LAYOUT_H
