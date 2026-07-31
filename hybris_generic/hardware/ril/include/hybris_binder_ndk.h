/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
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

#ifndef HYBRIS_BINDER_NDK_H
#define HYBRIS_BINDER_NDK_H

namespace OHOS {
namespace HybrisRil {

/*
 * Bind the libbinder_ndk C ABI to the Halium container's implementation.
 *
 * The AIDL code under aidl_gen/ is AOSP's own generated client/server code,
 * compiled here for OHOS/musl.  It calls libbinder_ndk's stable C ABI, which
 * only exists inside the container (bionic).  binder_ndk_trampolines.S
 * defines every one of those symbols as a tail jump through a slot table;
 * this call fills the table by dlsym'ing the container's libbinder_ndk.so
 * through libhybris.
 *
 * MUST be called — and must succeed — before any AIDL proxy, parcel or
 * binder object is touched.  Idempotent; safe from any thread.  Unresolved
 * slots are pointed at an abort handler that names the missing symbol rather
 * than jumping to NULL.
 */
bool BinderNdkInit();

/* Convenience wrappers so callers need not include <android/binder_process.h>
 * just to spin up a thread pool.  No-ops if BinderNdkInit() failed. */
void BinderNdkStartThreadPool(int maxThreads);

} // namespace HybrisRil
} // namespace OHOS

#endif // HYBRIS_BINDER_NDK_H
