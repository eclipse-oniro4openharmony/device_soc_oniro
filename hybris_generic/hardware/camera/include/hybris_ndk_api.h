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

#ifndef HYBRIS_CAMERA_NDK_API_H
#define HYBRIS_CAMERA_NDK_API_H

/*
 * The camera2 NDK (libcamera2ndk.so) and AImageReader (libmediandk.so) from
 * the Halium system image, resolved through libhybris into a plain function
 * table.  Nothing links against them; every call goes through this struct.
 *
 * The vendored NDK headers gate declarations on __ANDROID_API__ and annotate
 * them with __INTRODUCED_IN(), neither of which exists in the OHOS musl
 * headers — define both before pulling them in.
 */
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(x)
#endif
#ifndef __ANDROID_API__
#define __ANDROID_API__ 34
#endif

// The NDK headers assume a bionic prelude and use size_t/int types without
// pulling them in themselves.
#include <stddef.h>
#include <stdint.h>

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraError.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

namespace OHOS {
namespace HybrisCamera {

#define HYBRIS_NDK_CAMERA_FUNCS(X)              \
    X(ACameraManager_create)                    \
    X(ACameraManager_delete)                    \
    X(ACameraManager_getCameraIdList)           \
    X(ACameraManager_deleteCameraIdList)        \
    X(ACameraManager_getCameraCharacteristics)  \
    X(ACameraManager_openCamera)                \
    X(ACameraManager_registerAvailabilityCallback)   \
    X(ACameraManager_unregisterAvailabilityCallback) \
    X(ACameraMetadata_getConstEntry)            \
    X(ACameraMetadata_getAllTags)               \
    X(ACameraMetadata_free)                     \
    X(ACameraDevice_close)                      \
    X(ACameraDevice_getId)                      \
    X(ACameraDevice_createCaptureRequest)       \
    X(ACameraDevice_createCaptureSession)       \
    X(ACaptureSessionOutputContainer_create)    \
    X(ACaptureSessionOutputContainer_free)      \
    X(ACaptureSessionOutputContainer_add)       \
    X(ACaptureSessionOutputContainer_remove)    \
    X(ACaptureSessionOutput_create)             \
    X(ACaptureSessionOutput_free)               \
    X(ACameraOutputTarget_create)               \
    X(ACameraOutputTarget_free)                 \
    X(ACaptureRequest_addTarget)                \
    X(ACaptureRequest_removeTarget)             \
    X(ACaptureRequest_free)                     \
    X(ACaptureRequest_getConstEntry)            \
    X(ACaptureRequest_setEntry_i32)             \
    X(ACaptureRequest_setEntry_u8)              \
    X(ACaptureRequest_setEntry_float)           \
    X(ACaptureRequest_setEntry_i64)             \
    X(ACameraCaptureSession_setRepeatingRequest)\
    X(ACameraCaptureSession_stopRepeating)      \
    X(ACameraCaptureSession_abortCaptures)      \
    X(ACameraCaptureSession_capture)            \
    X(ACameraCaptureSession_close)

#define HYBRIS_NDK_MEDIA_FUNCS(X)   \
    X(AImageReader_new)             \
    X(AImageReader_delete)          \
    X(AImageReader_getWindow)       \
    X(AImageReader_setImageListener)\
    X(AImageReader_acquireNextImage)\
    X(AImageReader_acquireLatestImage) \
    X(AImage_delete)                \
    X(AImage_getWidth)              \
    X(AImage_getHeight)             \
    X(AImage_getFormat)             \
    X(AImage_getTimestamp)          \
    X(AImage_getNumberOfPlanes)     \
    X(AImage_getPlaneData)          \
    X(AImage_getPlaneRowStride)     \
    X(AImage_getPlanePixelStride)

struct NdkApi {
#define HYBRIS_NDK_DECL(name) decltype(&::name) name;
    HYBRIS_NDK_CAMERA_FUNCS(HYBRIS_NDK_DECL)
    HYBRIS_NDK_MEDIA_FUNCS(HYBRIS_NDK_DECL)
#undef HYBRIS_NDK_DECL
};

/*
 * Loads both libraries through the hybris linker and resolves the table.
 * Returns nullptr if anything is missing; the result is cached, so repeated
 * calls are cheap and a failure is not retried.
 *
 * NOTE: the process must run with HYBRIS_LD_LIBRARY_PATH putting
 * /android/system/lib64 *before* /android/vendor/lib64.  libcodec2_vndk.so
 * exists in both, and the vendor copy — which the default path finds first —
 * needs the VNDK build of libui, while everything else in this chain needs the
 * system build.  camera_host gets the right value from init.<hardware>.cfg.
 */
const NdkApi *LoadNdkApi();

// Human-readable camera_status_t, for logs.
const char *NdkStatusName(camera_status_t status);

} // namespace HybrisCamera
} // namespace OHOS

#endif // HYBRIS_CAMERA_NDK_API_H
