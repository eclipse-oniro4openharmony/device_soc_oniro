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

#ifndef HYBRIS_RIL_LOG_H
#define HYBRIS_RIL_LOG_H

/*
 * <syslog.h> arrives ahead of us through android/binder_internal_logging.h
 * (every AIDL header pulls it in) and defines LOG_INFO/LOG_DEBUG as macros,
 * which then eat the identically named enumerators of hilog's LogLevel.  Drop
 * the two that collide; the binder headers only ever use LOG_ERR, which hilog
 * does not declare (its error level is LOG_ERROR).
 */
#undef LOG_INFO
#undef LOG_DEBUG

#include <hilog/log.h>

/*
 * Own log domain.  Inside an HDF host neither stderr nor HDF_LOGx reach
 * anywhere useful (the audio bridge learned this the hard way), so every
 * module here logs through hilog with a domain of its own:
 *
 *     hdc shell "timeout 20 hilog -x | grep HybrisRil"
 */
#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002561
#define LOG_TAG "HybrisRil"

#define HR_LOGD(fmt, ...) HILOG_DEBUG(LOG_CORE, "%{public}s: " fmt, __func__, ##__VA_ARGS__)
#define HR_LOGI(fmt, ...) HILOG_INFO(LOG_CORE, "%{public}s: " fmt, __func__, ##__VA_ARGS__)
#define HR_LOGW(fmt, ...) HILOG_WARN(LOG_CORE, "%{public}s: " fmt, __func__, ##__VA_ARGS__)
#define HR_LOGE(fmt, ...) HILOG_ERROR(LOG_CORE, "%{public}s: " fmt, __func__, ##__VA_ARGS__)

#endif // HYBRIS_RIL_LOG_H
