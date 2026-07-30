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

#ifndef HYBRIS_CAMERA_SERVICE_PROXY_H
#define HYBRIS_CAMERA_SERVICE_PROXY_H

namespace OHOS {
namespace HybrisCamera {

using ProxyLogFn = void (*)(const char *msg);

/*
 * The Halium container runs AOSP's CameraService (inside minimediaservice) but
 * has no system_server, so the "media.camera.proxy" binder service that
 * CameraService consults never appears.  CameraServiceProxyWrapper fails
 * *closed* when the lookup misses, and every camera2 connectDevice() is
 * rejected with ERROR_DISABLED / "Camera disabled by device policy".
 *
 * EnsureCameraServiceProxy() registers a minimal, permissive stub of
 * android.hardware.ICameraServiceProxy so the check passes.  It uses
 * libbinder_ndk's stable C ABI, hosted by libhybris, so no Android C++ ABI
 * matching is involved.  Idempotent and safe to call from any process that
 * already talks to the container's binder.
 */
bool EnsureCameraServiceProxy();

/* Route this module's diagnostics somewhere useful (HDF_LOG in the VDI,
 * printf in the C0 harness).  Optional; defaults to stdout. */
void SetProxyLogger(ProxyLogFn fn);

} // namespace HybrisCamera
} // namespace OHOS

#endif // HYBRIS_CAMERA_SERVICE_PROXY_H
