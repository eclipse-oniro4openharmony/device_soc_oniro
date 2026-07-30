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

#ifndef HYBRIS_CAMERA_COMMON_H
#define HYBRIS_CAMERA_COMMON_H

#include <string>

// hilog with our own domain, same reasoning as the audio VDI: HDF_LOG* emits
// nothing from a VDI loaded into a driver host, and camera_host's stderr goes
// nowhere.  A bridge that fails silently inside a driver host is undebuggable.
#include "hilog/log.h"
#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "HybrisCamera"
#define LOG_DOMAIN 0xD001403
#define HC_LOGI(fmt, ...) HILOG_INFO(LOG_CORE, "[%{public}s] " fmt, __func__, ##__VA_ARGS__)
#define HC_LOGW(fmt, ...) HILOG_WARN(LOG_CORE, "[%{public}s] " fmt, __func__, ##__VA_ARGS__)
#define HC_LOGE(fmt, ...) HILOG_ERROR(LOG_CORE, "[%{public}s] " fmt, __func__, ##__VA_ARGS__)
#define HC_LOGD(fmt, ...) HILOG_DEBUG(LOG_CORE, "[%{public}s] " fmt, __func__, ##__VA_ARGS__)

namespace OHOS {
namespace HybrisCamera {

// Runtime gate.  One image serves both the X23 and the Plinius, so the VDI
// ships unconditionally and stays dormant (reports zero cameras) unless the
// per-device init.<hardware>.cfg sets this to "vdi".
constexpr const char *HDI_IMPL_PARAM = "ohos.camera.hdi_impl";
constexpr const char *HDI_IMPL_VDI = "vdi";

// androidd raises this once the container's HAL fleet is up.  The camera stack
// (minimediaservice + camerahalserver) comes up around the same time, so this
// is the earliest sensible moment to look for media.camera.
constexpr const char *ANDROID_READY_PARAM = "android.composer.ready";
constexpr int ANDROID_READY_TIMEOUT_S = 90;

// Stable VDI-side camera ids.  The HDI layer rewrites these into lcam00<N>
// before the framework ever sees them (camera_host_service.cpp
// vdiCameraIdToPrefix), so their only job is to be stable across boots.
constexpr const char *CAMERA_ID_BACK = "hybris_back";
constexpr const char *CAMERA_ID_FRONT = "hybris_front";

} // namespace HybrisCamera
} // namespace OHOS

#endif // HYBRIS_CAMERA_COMMON_H
