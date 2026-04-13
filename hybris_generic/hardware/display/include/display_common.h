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

/*
 * Bytes-per-pixel for a given OHOS PixelFormat.
 *
 * OHOS's BufferHandle::stride is in BYTES (see reference gralloc at
 * drivers/peripheral/display/hal/default_standard/src/display_gralloc/
 * allocator.cpp:166).  Android gralloc's hybris_gralloc_allocate returns
 * stride in PIXELS.  Both the buffer VDI (to convert pixel→byte stride on
 * allocation) and the composer VDI (to convert byte→pixel stride when
 * forwarding buffers to ANativeWindowBuffer) need this lookup, which is
 * why it lives in the shared display_common.h.
 *
 * Returns 0 for planar/sub-byte/unknown formats; callers treat that as "use
 * the pixel stride unchanged" to avoid breaking YUV camera buffers.
 */
static inline uint32_t HybrisBytesPerPixelOhos(uint32_t ohosFormat)
{
    switch (ohosFormat) {
        case 3:  /* RGB_565   */ return 2;
        case 11: /* RGBX_8888 */ return 4;
        case 12: /* RGBA_8888 */ return 4;
        case 13: /* RGB_888   */ return 3;
        case 14: /* BGR_565   */ return 2;
        case 19: /* BGRX_8888 */ return 4;
        case 20: /* BGRA_8888 */ return 4;
        default: /* YUV / sub-byte */ return 0;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* HYBRIS_DISPLAY_COMMON_H */
