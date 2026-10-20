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

#ifndef HYBRIS_GNSS_LOG_H
#define HYBRIS_GNSS_LOG_H

/* Same <syslog.h> collision as hybris_ril_log.h: the binder NDK headers
 * define LOG_INFO/LOG_DEBUG as macros, which eat hilog's enumerators. */
#undef LOG_INFO
#undef LOG_DEBUG

#include <hilog/log.h>

/*
 * Own log domain, matching the one BUILD.gn hands the shared binder shim:
 *
 *     hdc shell "timeout 20 hilog -x | grep HybrisGnss"
 */
#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002562
#define LOG_TAG "HybrisGnss"

#define HG_LOGD(fmt, ...) HILOG_DEBUG(LOG_CORE, "%{public}s: " fmt, __func__, ##__VA_ARGS__)
#define HG_LOGI(fmt, ...) HILOG_INFO(LOG_CORE, "%{public}s: " fmt, __func__, ##__VA_ARGS__)
#define HG_LOGW(fmt, ...) HILOG_WARN(LOG_CORE, "%{public}s: " fmt, __func__, ##__VA_ARGS__)
#define HG_LOGE(fmt, ...) HILOG_ERROR(LOG_CORE, "%{public}s: " fmt, __func__, ##__VA_ARGS__)

#endif // HYBRIS_GNSS_LOG_H
