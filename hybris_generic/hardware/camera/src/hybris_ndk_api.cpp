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

#include "hybris_ndk_api.h"

#include <cstring>
#include <mutex>

#include <dlfcn.h>

#include "hybris_dl.h"

#include "hybris_camera_common.h"

namespace OHOS {
namespace HybrisCamera {
namespace {

NdkApi g_api {};
bool g_loaded = false;
bool g_tried = false;
std::mutex g_loadLock;

} // namespace

const NdkApi *LoadNdkApi()
{
    std::lock_guard<std::mutex> lock(g_loadLock);
    if (g_tried) {
        return g_loaded ? &g_api : nullptr;
    }
    g_tried = true;

    void *cam = HybrisDlopen("libcamera2ndk.so", RTLD_LAZY);
    if (cam == nullptr) {
        HC_LOGE("HybrisDlopen(libcamera2ndk.so) failed: %{public}s", HybrisDlerror());
        return nullptr;
    }
    // Already pulled in as a NEEDED of libcamera2ndk; this just gets a handle
    // whose symbol table carries the AImageReader/AImage entry points.
    void *media = HybrisDlopen("libmediandk.so", RTLD_LAZY);
    if (media == nullptr) {
        HC_LOGE("HybrisDlopen(libmediandk.so) failed: %{public}s", HybrisDlerror());
        return nullptr;
    }

    bool ok = true;
#define HYBRIS_NDK_LOAD(handle, name)                                                  \
    do {                                                                               \
        g_api.name = reinterpret_cast<decltype(g_api.name)>(HybrisDlsym(handle, #name)); \
        if (g_api.name == nullptr) {                                                   \
            HC_LOGE("dlsym(%{public}s) failed", #name);                                \
            ok = false;                                                                \
        }                                                                              \
    } while (0);

#define HYBRIS_NDK_LOAD_CAM(name) HYBRIS_NDK_LOAD(cam, name)
#define HYBRIS_NDK_LOAD_MEDIA(name) HYBRIS_NDK_LOAD(media, name)
    HYBRIS_NDK_CAMERA_FUNCS(HYBRIS_NDK_LOAD_CAM)
    HYBRIS_NDK_MEDIA_FUNCS(HYBRIS_NDK_LOAD_MEDIA)
#undef HYBRIS_NDK_LOAD_MEDIA
#undef HYBRIS_NDK_LOAD_CAM
#undef HYBRIS_NDK_LOAD

    if (!ok) {
        HC_LOGE("camera2 NDK table incomplete — bridge unavailable");
        return nullptr;
    }
    g_loaded = true;
    HC_LOGI("camera2 NDK hosted: libcamera2ndk=%{public}p libmediandk=%{public}p", cam, media);
    return &g_api;
}

const char *NdkStatusName(camera_status_t status)
{
    switch (status) {
        case ACAMERA_OK: return "OK";
        case ACAMERA_ERROR_UNKNOWN: return "UNKNOWN";
        case ACAMERA_ERROR_INVALID_PARAMETER: return "INVALID_PARAMETER";
        case ACAMERA_ERROR_CAMERA_DISCONNECTED: return "CAMERA_DISCONNECTED";
        case ACAMERA_ERROR_NOT_ENOUGH_MEMORY: return "NOT_ENOUGH_MEMORY";
        case ACAMERA_ERROR_METADATA_NOT_FOUND: return "METADATA_NOT_FOUND";
        case ACAMERA_ERROR_CAMERA_DEVICE: return "CAMERA_DEVICE";
        case ACAMERA_ERROR_CAMERA_SERVICE: return "CAMERA_SERVICE";
        case ACAMERA_ERROR_SESSION_CLOSED: return "SESSION_CLOSED";
        case ACAMERA_ERROR_INVALID_OPERATION: return "INVALID_OPERATION";
        case ACAMERA_ERROR_STREAM_CONFIGURE_FAIL: return "STREAM_CONFIGURE_FAIL";
        case ACAMERA_ERROR_CAMERA_IN_USE: return "CAMERA_IN_USE";
        case ACAMERA_ERROR_MAX_CAMERA_IN_USE: return "MAX_CAMERA_IN_USE";
        case ACAMERA_ERROR_CAMERA_DISABLED: return "CAMERA_DISABLED";
        case ACAMERA_ERROR_PERMISSION_DENIED: return "PERMISSION_DENIED";
        case ACAMERA_ERROR_UNSUPPORTED_OPERATION: return "UNSUPPORTED_OPERATION";
        default: return "?";
    }
}

} // namespace HybrisCamera
} // namespace OHOS
