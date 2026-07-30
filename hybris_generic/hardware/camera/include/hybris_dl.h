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

#ifndef HYBRIS_CAMERA_DL_H
#define HYBRIS_CAMERA_DL_H

namespace OHOS {
namespace HybrisCamera {

/*
 * libhybris' Android loader, reached without a link-time dependency.
 *
 * The VDI installs to /vendor/lib64 (HdfLoadVdi looks nowhere else) and
 * deps_guard's passthrough rule will not let a module there link a
 * system-image library such as libhybris-common.  The audio VDI solves the
 * same problem the same way — dlopen the wrapper at runtime, which is itself
 * only a thin shim over the Android linker.
 */
void *HybrisDlopen(const char *filename, int flag);
void *HybrisDlsym(void *handle, const char *symbol);
const char *HybrisDlerror();

} // namespace HybrisCamera
} // namespace OHOS

#endif // HYBRIS_CAMERA_DL_H
