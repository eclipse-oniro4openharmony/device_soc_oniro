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

/*
 * The part of Android's NDK media C API (libmediandk.so: AMediaCodec,
 * AMediaFormat) the AVC encoder shim uses.
 *
 * The library lives in the Halium system image and is reached through
 * libhybris, so nothing is linked: every entry point is resolved with
 * hybris_dlsym.  This is the stable NDK ABI (API level 28 for
 * getInputFormat, 26 for setParameters), the same on every Android release
 * the Halium images are built from.
 */

#ifndef ONIRO_AVCENC_MEDIANDK_ABI_H
#define ONIRO_AVCENC_MEDIANDK_ABI_H

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

extern "C" {

typedef struct AMediaCodec AMediaCodec;
typedef struct AMediaFormat AMediaFormat;
typedef int32_t media_status_t; /* AMEDIA_OK == 0 */

typedef struct AMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
} AMediaCodecBufferInfo;

} // extern "C"

namespace OHOS {
namespace OniroAvcEnc {

constexpr media_status_t AMEDIA_OK = 0;
constexpr uint32_t AMEDIACODEC_CONFIGURE_FLAG_ENCODE = 1;
constexpr uint32_t AMEDIACODEC_BUFFER_FLAG_KEY_FRAME = 1;
constexpr uint32_t AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG = 2;
constexpr uint32_t AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4;
constexpr ssize_t AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1;
constexpr ssize_t AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2;
constexpr ssize_t AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3;

/* Entry points, resolved at runtime. */
struct MediaNdk {
    AMediaCodec *(*createCodecByName)(const char *name) = nullptr;
    AMediaCodec *(*createEncoderByType)(const char *mime) = nullptr;
    media_status_t (*configure)(AMediaCodec *, const AMediaFormat *, void *surface, void *crypto,
                                uint32_t flags) = nullptr;
    media_status_t (*start)(AMediaCodec *) = nullptr;
    media_status_t (*stop)(AMediaCodec *) = nullptr;
    media_status_t (*destroy)(AMediaCodec *) = nullptr; /* AMediaCodec_delete */
    ssize_t (*dequeueInputBuffer)(AMediaCodec *, int64_t timeoutUs) = nullptr;
    uint8_t *(*getInputBuffer)(AMediaCodec *, size_t idx, size_t *outSize) = nullptr;
    media_status_t (*queueInputBuffer)(AMediaCodec *, size_t idx, off_t offset, size_t size, uint64_t timeUs,
                                       uint32_t flags) = nullptr;
    ssize_t (*dequeueOutputBuffer)(AMediaCodec *, AMediaCodecBufferInfo *, int64_t timeoutUs) = nullptr;
    uint8_t *(*getOutputBuffer)(AMediaCodec *, size_t idx, size_t *outSize) = nullptr;
    media_status_t (*releaseOutputBuffer)(AMediaCodec *, size_t idx, bool render) = nullptr;
    AMediaFormat *(*getInputFormat)(AMediaCodec *) = nullptr;
    media_status_t (*setParameters)(AMediaCodec *, const AMediaFormat *) = nullptr;

    AMediaFormat *(*formatNew)() = nullptr;
    media_status_t (*formatDelete)(AMediaFormat *) = nullptr;
    void (*formatSetString)(AMediaFormat *, const char *name, const char *value) = nullptr;
    void (*formatSetInt32)(AMediaFormat *, const char *name, int32_t value) = nullptr;
    bool (*formatGetInt32)(AMediaFormat *, const char *name, int32_t *out) = nullptr;
    const char *(*formatToString)(AMediaFormat *) = nullptr;
};

/* Loads libmediandk.so through libhybris once per process; null on failure. */
const MediaNdk *LoadMediaNdk();

} // namespace OniroAvcEnc
} // namespace OHOS

#endif // ONIRO_AVCENC_MEDIANDK_ABI_H
