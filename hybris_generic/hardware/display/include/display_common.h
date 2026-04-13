/*
 * Copyright (c) 2024 Oniro Authors
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

#ifndef HYBRIS_DISPLAY_COMMON_H
#define HYBRIS_DISPLAY_COMMON_H

#include <string.h>
#include <stdint.h>
#include "hilog/log.h"
#include "stdio.h"

#ifdef HDF_LOG_TAG
#undef HDF_LOG_TAG
#endif

#if defined(__cplusplus)
extern "C" {
#endif

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "HybrisDisp"
#define LOG_DOMAIN 0xD001400

#ifndef DISPLAY_UNUSED
#define DISPLAY_UNUSED(x) (void)x
#endif

#define __FILENAME__ (strrchr(__FILE__, '/') ? (strrchr(__FILE__, '/') + 1) : __FILE__)

#ifndef DISPLAY_LOGD
#define DISPLAY_LOGD(format, ...) \
    HILOG_DEBUG(LOG_CORE, "[%{public}s@%{public}s:%{public}d] " format "\n", \
        __FUNCTION__, __FILENAME__, __LINE__, ##__VA_ARGS__)
#endif

#ifndef DISPLAY_LOGI
#define DISPLAY_LOGI(format, ...) \
    HILOG_INFO(LOG_CORE, "[%{public}s@%{public}s:%{public}d] " format "\n", \
        __FUNCTION__, __FILENAME__, __LINE__, ##__VA_ARGS__)
#endif

#ifndef DISPLAY_LOGW
#define DISPLAY_LOGW(format, ...) \
    HILOG_WARN(LOG_CORE, "[%{public}s@%{public}s:%{public}d] " format "\n", \
        __FUNCTION__, __FILENAME__, __LINE__, ##__VA_ARGS__)
#endif

#ifndef DISPLAY_LOGE
#define DISPLAY_LOGE(format, ...) \
    HILOG_ERROR(LOG_CORE, "[%{public}s@%{public}s:%{public}d] " format "\n", \
        __FUNCTION__, __FILENAME__, __LINE__, ##__VA_ARGS__)
#endif

#ifndef DISPLAY_CHK_RETURN
#define DISPLAY_CHK_RETURN(val, ret, ...) \
    do { \
        if (val) { \
            __VA_ARGS__; \
            return (ret); \
        } \
    } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* HYBRIS_DISPLAY_COMMON_H */
