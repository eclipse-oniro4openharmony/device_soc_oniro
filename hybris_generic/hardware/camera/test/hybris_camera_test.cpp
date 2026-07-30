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
 * hybris_camera_test — phase C0 harness of the camera enablement plan
 * (device/board/oniro/docs/hybris_generic/camera_enablement_plan.md).
 *
 * Runs as an ordinary OHOS process and drives the Android CameraService
 * living in the androidd container through the camera2 NDK, hosted here by
 * libhybris.  Everything the NDK needs (binder to media.camera, gralloc,
 * the MTK provider) is already up; this harness answers C0's go/no-go:
 *
 *   - does libcamera2ndk.so + libmediandk.so load under the hybris linker?
 *   - does CameraService accept connectDevice from an OHOS uid?
 *   - do YUV_420_888 frames and a JPEG blob actually come back?
 *
 * It also dumps the characteristics that feed the D4 ability-mapping table,
 * so C1 does not have to guess what the HAL advertises.
 *
 * Usage (see usage() at the bottom):
 *   hybris_camera_test --list
 *   hybris_camera_test --cam 0 --frames 60 --jpeg
 *   hybris_camera_test --uid 3028 --gid 3028 --cam 0 --frames 30
 */

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <hybris/common/dlfcn.h>

#include "hybris_camera_service_proxy.h"

/*
 * The vendored NDK headers gate every declaration on __ANDROID_API__ and
 * annotate it with __INTRODUCED_IN(), neither of which exists in the OHOS
 * musl headers.  We only need the types, enums and function signatures —
 * every entry point is resolved through hybris_dlsym() below.
 */
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(x)
#endif
#ifndef __ANDROID_API__
#define __ANDROID_API__ 34
#endif

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraError.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

// ─── logging ────────────────────────────────────────────────────────────────

static double NowMs()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static double g_t0 = 0;

static void Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[%8.1f] ", NowMs() - g_t0);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

// ─── hybris-resolved entry points ───────────────────────────────────────────
//
// decltype(&::Fn) keeps every pointer type-checked against the vendored
// header, so a signature drift is a compile error rather than a stack smash.

#define NDK_FN(name) static decltype(&::name) p_##name = nullptr

NDK_FN(ACameraManager_create);
NDK_FN(ACameraManager_delete);
NDK_FN(ACameraManager_getCameraIdList);
NDK_FN(ACameraManager_deleteCameraIdList);
NDK_FN(ACameraManager_getCameraCharacteristics);
NDK_FN(ACameraManager_openCamera);
NDK_FN(ACameraManager_registerAvailabilityCallback);
NDK_FN(ACameraManager_unregisterAvailabilityCallback);
NDK_FN(ACameraMetadata_getConstEntry);
NDK_FN(ACameraMetadata_getAllTags);
NDK_FN(ACameraMetadata_free);
NDK_FN(ACameraDevice_close);
NDK_FN(ACameraDevice_getId);
NDK_FN(ACameraDevice_createCaptureRequest);
NDK_FN(ACameraDevice_createCaptureSession);
NDK_FN(ACaptureSessionOutputContainer_create);
NDK_FN(ACaptureSessionOutputContainer_free);
NDK_FN(ACaptureSessionOutputContainer_add);
NDK_FN(ACaptureSessionOutput_create);
NDK_FN(ACaptureSessionOutput_free);
NDK_FN(ACameraOutputTarget_create);
NDK_FN(ACameraOutputTarget_free);
NDK_FN(ACaptureRequest_addTarget);
NDK_FN(ACaptureRequest_free);
NDK_FN(ACaptureRequest_setEntry_i32);
NDK_FN(ACaptureRequest_setEntry_u8);
NDK_FN(ACameraCaptureSession_setRepeatingRequest);
NDK_FN(ACameraCaptureSession_stopRepeating);
NDK_FN(ACameraCaptureSession_capture);
NDK_FN(ACameraCaptureSession_close);

NDK_FN(AImageReader_new);
NDK_FN(AImageReader_delete);
NDK_FN(AImageReader_getWindow);
NDK_FN(AImageReader_setImageListener);
NDK_FN(AImageReader_acquireNextImage);
NDK_FN(AImage_delete);
NDK_FN(AImage_getWidth);
NDK_FN(AImage_getHeight);
NDK_FN(AImage_getFormat);
NDK_FN(AImage_getTimestamp);
NDK_FN(AImage_getNumberOfPlanes);
NDK_FN(AImage_getPlaneData);
NDK_FN(AImage_getPlaneRowStride);
NDK_FN(AImage_getPlanePixelStride);

static bool g_loadOk = true;

#define LOAD_FN(handle, name)                                                          \
    do {                                                                               \
        p_##name = reinterpret_cast<decltype(p_##name)>(hybris_dlsym(handle, #name));   \
        if (p_##name == nullptr) {                                                     \
            Log("  dlsym FAILED: %s", #name);                                          \
            g_loadOk = false;                                                          \
        }                                                                              \
    } while (0)

static bool LoadNdk()
{
    Log("hybris_dlopen(libcamera2ndk.so)…");
    void *cam = hybris_dlopen("libcamera2ndk.so", RTLD_LAZY);
    if (cam == nullptr) {
        Log("FATAL: libcamera2ndk.so failed to load: %s", hybris_dlerror());
        return false;
    }
    Log("  libcamera2ndk.so @ %p", cam);

    Log("hybris_dlopen(libmediandk.so)…");
    void *media = hybris_dlopen("libmediandk.so", RTLD_LAZY);
    if (media == nullptr) {
        Log("FATAL: libmediandk.so failed to load: %s", hybris_dlerror());
        return false;
    }
    Log("  libmediandk.so @ %p", media);

    LOAD_FN(cam, ACameraManager_create);
    LOAD_FN(cam, ACameraManager_delete);
    LOAD_FN(cam, ACameraManager_getCameraIdList);
    LOAD_FN(cam, ACameraManager_deleteCameraIdList);
    LOAD_FN(cam, ACameraManager_getCameraCharacteristics);
    LOAD_FN(cam, ACameraManager_openCamera);
    LOAD_FN(cam, ACameraManager_registerAvailabilityCallback);
    LOAD_FN(cam, ACameraManager_unregisterAvailabilityCallback);
    LOAD_FN(cam, ACameraMetadata_getConstEntry);
    LOAD_FN(cam, ACameraMetadata_getAllTags);
    LOAD_FN(cam, ACameraMetadata_free);
    LOAD_FN(cam, ACameraDevice_close);
    LOAD_FN(cam, ACameraDevice_getId);
    LOAD_FN(cam, ACameraDevice_createCaptureRequest);
    LOAD_FN(cam, ACameraDevice_createCaptureSession);
    LOAD_FN(cam, ACaptureSessionOutputContainer_create);
    LOAD_FN(cam, ACaptureSessionOutputContainer_free);
    LOAD_FN(cam, ACaptureSessionOutputContainer_add);
    LOAD_FN(cam, ACaptureSessionOutput_create);
    LOAD_FN(cam, ACaptureSessionOutput_free);
    LOAD_FN(cam, ACameraOutputTarget_create);
    LOAD_FN(cam, ACameraOutputTarget_free);
    LOAD_FN(cam, ACaptureRequest_addTarget);
    LOAD_FN(cam, ACaptureRequest_free);
    LOAD_FN(cam, ACaptureRequest_setEntry_i32);
    LOAD_FN(cam, ACaptureRequest_setEntry_u8);
    LOAD_FN(cam, ACameraCaptureSession_setRepeatingRequest);
    LOAD_FN(cam, ACameraCaptureSession_stopRepeating);
    LOAD_FN(cam, ACameraCaptureSession_capture);
    LOAD_FN(cam, ACameraCaptureSession_close);

    LOAD_FN(media, AImageReader_new);
    LOAD_FN(media, AImageReader_delete);
    LOAD_FN(media, AImageReader_getWindow);
    LOAD_FN(media, AImageReader_setImageListener);
    LOAD_FN(media, AImageReader_acquireNextImage);
    LOAD_FN(media, AImage_delete);
    LOAD_FN(media, AImage_getWidth);
    LOAD_FN(media, AImage_getHeight);
    LOAD_FN(media, AImage_getFormat);
    LOAD_FN(media, AImage_getTimestamp);
    LOAD_FN(media, AImage_getNumberOfPlanes);
    LOAD_FN(media, AImage_getPlaneData);
    LOAD_FN(media, AImage_getPlaneRowStride);
    LOAD_FN(media, AImage_getPlanePixelStride);

    return g_loadOk;
}

// ─── metadata dumping (feeds the D4 ability table) ──────────────────────────

static const char *StatusName(camera_status_t s)
{
    switch (s) {
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

static const char *FormatName(int32_t f)
{
    switch (f) {
        case 0x20: return "RAW16";
        case 0x21: return "BLOB/JPEG";
        case 0x22: return "IMPLEMENTATION_DEFINED";
        case 0x23: return "YCbCr_420_888";
        case 0x24: return "RAW_OPAQUE";
        case 0x25: return "RAW10";
        case 0x26: return "RAW12";
        case 0x32315659: return "YV12";
        case 0x11: return "YCrCb_420_SP/NV21";
        case 0x3: return "RGB_888";
        case 0x1: return "RGBA_8888";
        case 0x26c: return "JPEG_R";
        case 0x38: return "Y8";
        case 0x1002: return "DEPTH16";
        case 0x1003: return "DEPTH_POINT_CLOUD";
        default: return "?";
    }
}

struct TagDesc {
    uint32_t tag;
    const char *name;
};

// The subset the VDI's GetCameraAbility() has to synthesize (plan §D4).
static const TagDesc kAbilityTags[] = {
    { ACAMERA_LENS_FACING, "LENS_FACING" },
    { ACAMERA_SENSOR_ORIENTATION, "SENSOR_ORIENTATION" },
    { ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, "INFO_SUPPORTED_HARDWARE_LEVEL" },
    { ACAMERA_FLASH_INFO_AVAILABLE, "FLASH_INFO_AVAILABLE" },
    { ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, "REQUEST_AVAILABLE_CAPABILITIES" },
    { ACAMERA_REQUEST_PARTIAL_RESULT_COUNT, "REQUEST_PARTIAL_RESULT_COUNT" },
    { ACAMERA_REQUEST_PIPELINE_MAX_DEPTH, "REQUEST_PIPELINE_MAX_DEPTH" },
    { ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, "SENSOR_INFO_ACTIVE_ARRAY_SIZE" },
    { ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, "SENSOR_INFO_PIXEL_ARRAY_SIZE" },
    { ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, "SENSOR_INFO_PHYSICAL_SIZE" },
    { ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, "LENS_INFO_AVAILABLE_FOCAL_LENGTHS" },
    { ACAMERA_LENS_INFO_AVAILABLE_APERTURES, "LENS_INFO_AVAILABLE_APERTURES" },
    { ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, "LENS_INFO_MINIMUM_FOCUS_DISTANCE" },
    { ACAMERA_CONTROL_AE_AVAILABLE_MODES, "CONTROL_AE_AVAILABLE_MODES" },
    { ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, "CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES" },
    { ACAMERA_CONTROL_AE_COMPENSATION_RANGE, "CONTROL_AE_COMPENSATION_RANGE" },
    { ACAMERA_CONTROL_AE_COMPENSATION_STEP, "CONTROL_AE_COMPENSATION_STEP" },
    { ACAMERA_CONTROL_AF_AVAILABLE_MODES, "CONTROL_AF_AVAILABLE_MODES" },
    { ACAMERA_CONTROL_AWB_AVAILABLE_MODES, "CONTROL_AWB_AVAILABLE_MODES" },
    { ACAMERA_CONTROL_AVAILABLE_EFFECTS, "CONTROL_AVAILABLE_EFFECTS" },
    { ACAMERA_CONTROL_AVAILABLE_SCENE_MODES, "CONTROL_AVAILABLE_SCENE_MODES" },
    { ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, "CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES" },
    { ACAMERA_CONTROL_MAX_REGIONS, "CONTROL_MAX_REGIONS" },
    { ACAMERA_CONTROL_ZOOM_RATIO_RANGE, "CONTROL_ZOOM_RATIO_RANGE" },
    { ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, "SCALER_AVAILABLE_MAX_DIGITAL_ZOOM" },
    { ACAMERA_JPEG_AVAILABLE_THUMBNAIL_SIZES, "JPEG_AVAILABLE_THUMBNAIL_SIZES" },
    { ACAMERA_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, "STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES" },
    { ACAMERA_SYNC_MAX_LATENCY, "SYNC_MAX_LATENCY" },
};

static void PrintEntry(const ACameraMetadata_const_entry &e, const char *indent)
{
    const uint32_t kMaxPrint = 64;
    uint32_t n = e.count < kMaxPrint ? e.count : kMaxPrint;
    printf("%s", indent);
    for (uint32_t i = 0; i < n; ++i) {
        switch (e.type) {
            case ACAMERA_TYPE_BYTE:   printf("%u ", e.data.u8[i]); break;
            case ACAMERA_TYPE_INT32:  printf("%d ", e.data.i32[i]); break;
            case ACAMERA_TYPE_FLOAT:  printf("%g ", e.data.f[i]); break;
            case ACAMERA_TYPE_INT64:  printf("%lld ", static_cast<long long>(e.data.i64[i])); break;
            case ACAMERA_TYPE_DOUBLE: printf("%g ", e.data.d[i]); break;
            case ACAMERA_TYPE_RATIONAL:
                printf("%d/%d ", e.data.r[i].numerator, e.data.r[i].denominator);
                break;
            default: printf("? "); break;
        }
    }
    if (e.count > n) {
        printf("… (+%u)", e.count - n);
    }
    printf("\n");
    fflush(stdout);
}

static void DumpTag(const ACameraMetadata *meta, uint32_t tag, const char *name)
{
    ACameraMetadata_const_entry e {};
    camera_status_t st = p_ACameraMetadata_getConstEntry(meta, tag, &e);
    if (st != ACAMERA_OK) {
        printf("    %-44s <absent>\n", name);
        return;
    }
    printf("    %-44s type=%u count=%u\n", name, e.type, e.count);
    PrintEntry(e, "      ");
}

// Stream configurations, grouped by format — the table C1 must translate into
// OHOS_ABILITY_STREAM_AVAILABLE_BASIC_CONFIGURATIONS.
static void DumpStreamConfigs(const ACameraMetadata *meta)
{
    ACameraMetadata_const_entry e {};
    camera_status_t st = p_ACameraMetadata_getConstEntry(
        meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e);
    if (st != ACAMERA_OK) {
        printf("    SCALER_AVAILABLE_STREAM_CONFIGURATIONS <absent>\n");
        return;
    }
    printf("    SCALER_AVAILABLE_STREAM_CONFIGURATIONS (%u entries)\n", e.count / 4);
    std::vector<int32_t> formats;
    for (uint32_t i = 0; i + 3 < e.count; i += 4) {
        int32_t fmt = e.data.i32[i];
        bool known = false;
        for (int32_t f : formats) {
            if (f == fmt) { known = true; break; }
        }
        if (!known) { formats.push_back(fmt); }
    }
    for (int32_t fmt : formats) {
        printf("      format 0x%x (%s):", fmt, FormatName(fmt));
        int printed = 0;
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            if (e.data.i32[i] != fmt || e.data.i32[i + 3] != 0 /* output */) {
                continue;
            }
            printf(" %dx%d", e.data.i32[i + 1], e.data.i32[i + 2]);
            if (++printed >= 40) { printf(" …"); break; }
        }
        printf("\n");
    }
    fflush(stdout);
}

static void DumpCharacteristics(ACameraManager *mgr, const char *id, bool dumpAll)
{
    ACameraMetadata *meta = nullptr;
    camera_status_t st = p_ACameraManager_getCameraCharacteristics(mgr, id, &meta);
    if (st != ACAMERA_OK) {
        Log("  getCameraCharacteristics(%s) -> %s", id, StatusName(st));
        return;
    }
    printf("  === camera \"%s\" characteristics ===\n", id);
    for (const TagDesc &t : kAbilityTags) {
        DumpTag(meta, t.tag, t.name);
    }
    DumpStreamConfigs(meta);

    if (dumpAll) {
        const uint32_t *tags = nullptr;
        int32_t count = 0;
        if (p_ACameraMetadata_getAllTags(meta, &count, &tags) == ACAMERA_OK) {
            printf("    --- all %d tags ---\n", count);
            for (int32_t i = 0; i < count; ++i) {
                ACameraMetadata_const_entry e {};
                if (p_ACameraMetadata_getConstEntry(meta, tags[i], &e) != ACAMERA_OK) {
                    continue;
                }
                printf("    tag 0x%08x sect=%u type=%u count=%u\n",
                       tags[i], tags[i] >> 16, e.type, e.count);
                if (e.count <= 32) {
                    PrintEntry(e, "      ");
                }
            }
        }
    }
    p_ACameraMetadata_free(meta);
    fflush(stdout);
}

// ─── capture session state ──────────────────────────────────────────────────

struct Harness {
    std::mutex lock;
    std::condition_variable cv;

    bool deviceError = false;
    bool deviceDisconnected = false;
    bool sessionReady = false;
    bool sessionActive = false;

    uint32_t previewFrames = 0;
    uint32_t previewDropped = 0;
    double firstFrameMs = 0;
    double lastFrameMs = 0;
    bool planesLogged = false;

    bool jpegDone = false;
    std::string jpegPath = "/data/c0.jpg";
    size_t jpegBytes = 0;

    uint32_t captureStarted = 0;
    uint32_t captureCompleted = 0;
    uint32_t captureFailed = 0;
};

static Harness g_h;

// device callbacks -----------------------------------------------------------

static void OnDeviceDisconnected(void *, ACameraDevice *dev)
{
    Log("!! device DISCONNECTED (%s)", p_ACameraDevice_getId(dev));
    std::lock_guard<std::mutex> l(g_h.lock);
    g_h.deviceDisconnected = true;
    g_h.cv.notify_all();
}

static void OnDeviceError(void *, ACameraDevice *dev, int error)
{
    Log("!! device ERROR %d (%s)", error, p_ACameraDevice_getId(dev));
    std::lock_guard<std::mutex> l(g_h.lock);
    g_h.deviceError = true;
    g_h.cv.notify_all();
}

// session callbacks ----------------------------------------------------------

static void OnSessionClosed(void *, ACameraCaptureSession *)
{
    Log("session closed");
}

static void OnSessionReady(void *, ACameraCaptureSession *)
{
    Log("session ready");
    std::lock_guard<std::mutex> l(g_h.lock);
    g_h.sessionReady = true;
    g_h.cv.notify_all();
}

static void OnSessionActive(void *, ACameraCaptureSession *)
{
    Log("session active");
    std::lock_guard<std::mutex> l(g_h.lock);
    g_h.sessionActive = true;
    g_h.cv.notify_all();
}

// capture callbacks ----------------------------------------------------------

static void OnCaptureStarted(void *, ACameraCaptureSession *, const ACaptureRequest *, int64_t ts)
{
    std::lock_guard<std::mutex> l(g_h.lock);
    if (g_h.captureStarted < 3) {
        Log("onCaptureStarted ts=%lld", static_cast<long long>(ts));
    }
    g_h.captureStarted++;
}

static void OnCaptureCompleted(void *, ACameraCaptureSession *, ACaptureRequest *,
                               const ACameraMetadata *result)
{
    std::lock_guard<std::mutex> l(g_h.lock);
    g_h.captureCompleted++;
    if (g_h.captureCompleted == 1) {
        ACameraMetadata_const_entry e {};
        int64_t ts = -1;
        uint8_t afState = 0xff;
        uint8_t aeState = 0xff;
        if (p_ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK) {
            ts = e.data.i64[0];
        }
        if (p_ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AF_STATE, &e) == ACAMERA_OK) {
            afState = e.data.u8[0];
        }
        if (p_ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AE_STATE, &e) == ACAMERA_OK) {
            aeState = e.data.u8[0];
        }
        Log("onCaptureCompleted #1: sensorTs=%lld afState=%u aeState=%u",
            static_cast<long long>(ts), afState, aeState);
    }
}

static void OnCaptureFailed(void *, ACameraCaptureSession *, ACaptureRequest *,
                            ACameraCaptureFailure *failure)
{
    std::lock_guard<std::mutex> l(g_h.lock);
    g_h.captureFailed++;
    if (g_h.captureFailed <= 3) {
        Log("!! onCaptureFailed seq=%d frame=%lld imageCaptured=%d",
            failure->sequenceId, static_cast<long long>(failure->frameNumber),
            failure->wasImageCaptured);
    }
}

static void OnSequenceCompleted(void *, ACameraCaptureSession *, int seqId, int64_t frame)
{
    Log("sequence %d completed (last frame %lld)", seqId, static_cast<long long>(frame));
}

static void OnSequenceAborted(void *, ACameraCaptureSession *, int seqId)
{
    Log("sequence %d aborted", seqId);
}

static void OnBufferLost(void *, ACameraCaptureSession *, ACaptureRequest *,
                         ACameraWindowType *, int64_t frame)
{
    Log("!! buffer lost, frame %lld", static_cast<long long>(frame));
}

// image listeners ------------------------------------------------------------

static void OnPreviewImage(void *, AImageReader *reader)
{
    AImage *img = nullptr;
    media_status_t st = p_AImageReader_acquireNextImage(reader, &img);
    if (st != AMEDIA_OK || img == nullptr) {
        std::lock_guard<std::mutex> l(g_h.lock);
        g_h.previewDropped++;
        return;
    }

    double now = NowMs();
    bool logPlanes = false;
    {
        std::lock_guard<std::mutex> l(g_h.lock);
        if (g_h.previewFrames == 0) {
            g_h.firstFrameMs = now;
            logPlanes = !g_h.planesLogged;
            g_h.planesLogged = true;
        }
        g_h.previewFrames++;
        g_h.lastFrameMs = now;
        g_h.cv.notify_all();
    }

    if (logPlanes) {
        int32_t w = 0;
        int32_t h = 0;
        int32_t fmt = 0;
        int32_t planes = 0;
        int64_t ts = 0;
        p_AImage_getWidth(img, &w);
        p_AImage_getHeight(img, &h);
        p_AImage_getFormat(img, &fmt);
        p_AImage_getNumberOfPlanes(img, &planes);
        p_AImage_getTimestamp(img, &ts);
        Log("first preview frame: %dx%d fmt=0x%x (%s) planes=%d ts=%lld",
            w, h, fmt, FormatName(fmt), planes, static_cast<long long>(ts));
        // Plane geometry decides the D3 copy loop (NV12 vs NV21 vs planar).
        uint8_t *base[3] = { nullptr, nullptr, nullptr };
        for (int32_t p = 0; p < planes && p < 3; ++p) {
            uint8_t *data = nullptr;
            int len = 0;
            int32_t rowStride = 0;
            int32_t pixStride = 0;
            p_AImage_getPlaneData(img, p, &data, &len);
            p_AImage_getPlaneRowStride(img, p, &rowStride);
            p_AImage_getPlanePixelStride(img, p, &pixStride);
            base[p] = data;
            Log("  plane %d: data=%p len=%d rowStride=%d pixelStride=%d",
                p, data, len, rowStride, pixStride);
        }
        if (planes >= 3 && base[1] != nullptr && base[2] != nullptr) {
            // Semi-planar HALs alias U/V into one buffer; the sign of the
            // delta tells NV12 (V after U) from NV21 (U after V).
            long delta = static_cast<long>(base[2] - base[1]);
            Log("  chroma layout: plane2-plane1 = %ld  => %s", delta,
                delta == 1 ? "NV12 (U first, VU interleaved as CbCr)"
                           : (delta == -1 ? "NV21 (V first — matches OHOS PIXEL_FMT_YCRCB_420_SP)"
                                          : "fully planar (I420-like)"));
        }
    }

    p_AImage_delete(img);
}

static void OnJpegImage(void *, AImageReader *reader)
{
    AImage *img = nullptr;
    media_status_t st = p_AImageReader_acquireNextImage(reader, &img);
    if (st != AMEDIA_OK || img == nullptr) {
        Log("!! JPEG acquire failed: %d", st);
        return;
    }
    uint8_t *data = nullptr;
    int len = 0;
    int32_t w = 0;
    int32_t h = 0;
    int64_t ts = 0;
    p_AImage_getWidth(img, &w);
    p_AImage_getHeight(img, &h);
    p_AImage_getTimestamp(img, &ts);
    p_AImage_getPlaneData(img, 0, &data, &len);
    Log("JPEG image: reader %dx%d, blob %d bytes, ts=%lld", w, h, len,
        static_cast<long long>(ts));

    // The BLOB buffer is padded; the real JPEG ends at the EOI marker.
    int jpegLen = len;
    if (data != nullptr && len > 4) {
        for (int i = len - 2; i >= 2; --i) {
            if (data[i] == 0xff && data[i + 1] == 0xd9) {
                jpegLen = i + 2;
                break;
            }
        }
    }

    if (data != nullptr && jpegLen > 0) {
        FILE *f = fopen(g_h.jpegPath.c_str(), "wb");
        if (f != nullptr) {
            size_t wrote = fwrite(data, 1, static_cast<size_t>(jpegLen), f);
            fclose(f);
            Log("wrote %zu bytes to %s (trimmed from %d at EOI)", wrote,
                g_h.jpegPath.c_str(), len);
            std::lock_guard<std::mutex> l(g_h.lock);
            g_h.jpegBytes = wrote;
        } else {
            Log("!! fopen(%s) failed: %s", g_h.jpegPath.c_str(), strerror(errno));
        }
    }
    p_AImage_delete(img);

    std::lock_guard<std::mutex> l(g_h.lock);
    g_h.jpegDone = true;
    g_h.cv.notify_all();
}

// availability callbacks -----------------------------------------------------

static void OnCameraAvailable(void *, const char *id)
{
    Log("availability: camera %s AVAILABLE", id);
}

static void OnCameraUnavailable(void *, const char *id)
{
    Log("availability: camera %s UNAVAILABLE", id);
}

// ─── the actual run ─────────────────────────────────────────────────────────

struct Options {
    const char *camId = "0";
    int32_t width = 1280;
    int32_t height = 720;
    int32_t jpegWidth = 0;   // 0 = pick the largest sane BLOB size
    int32_t jpegHeight = 0;
    uint32_t frames = 60;
    bool jpeg = true;
    bool listOnly = false;
    bool dumpAll = false;
    int soakSec = 0;
    int reopen = 1;
    int availSec = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    bool dropPriv = false;
    bool proxy = true;
    const char *out = "/data/c0.jpg";
};

// Pick a JPEG size that will not stress the pipeline during bring-up: the
// largest BLOB configuration at or below 4096 wide (plan §D4 caps stills).
static bool PickJpegSize(ACameraManager *mgr, const char *id, int32_t *w, int32_t *h)
{
    ACameraMetadata *meta = nullptr;
    if (p_ACameraManager_getCameraCharacteristics(mgr, id, &meta) != ACAMERA_OK) {
        return false;
    }
    ACameraMetadata_const_entry e {};
    bool found = false;
    if (p_ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                        &e) == ACAMERA_OK) {
        int64_t best = 0;
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            if (e.data.i32[i] != 0x21 || e.data.i32[i + 3] != 0) {
                continue;
            }
            int32_t cw = e.data.i32[i + 1];
            int32_t ch = e.data.i32[i + 2];
            if (cw > 4096) {
                continue;
            }
            int64_t area = static_cast<int64_t>(cw) * ch;
            if (area > best) {
                best = area;
                *w = cw;
                *h = ch;
                found = true;
            }
        }
    }
    p_ACameraMetadata_free(meta);
    return found;
}

static bool RunOnce(ACameraManager *mgr, const Options &opt, int iteration)
{
    Log("--- open cycle %d: camera \"%s\" ---", iteration, opt.camId);

    ACameraDevice_StateCallbacks devCb {};
    devCb.context = nullptr;
    devCb.onDisconnected = OnDeviceDisconnected;
    devCb.onError = OnDeviceError;

    ACameraDevice *dev = nullptr;
    double t = NowMs();
    camera_status_t st = p_ACameraManager_openCamera(mgr, opt.camId, &devCb, &dev);
    Log("openCamera(\"%s\") -> %s (%.0f ms)", opt.camId, StatusName(st), NowMs() - t);
    if (st != ACAMERA_OK || dev == nullptr) {
        Log("*** C0 GATE: openCamera FAILED — this is the permission/posture answer ***");
        return false;
    }
    Log("*** C0 GATE: openCamera SUCCEEDED from uid=%u ***", getuid());

    // Preview reader ---------------------------------------------------------
    AImageReader *previewReader = nullptr;
    media_status_t ms = p_AImageReader_new(opt.width, opt.height, AIMAGE_FORMAT_YUV_420_888,
                                           /*maxImages=*/4, &previewReader);
    if (ms != AMEDIA_OK) {
        Log("!! AImageReader_new(preview %dx%d) failed: %d", opt.width, opt.height, ms);
        p_ACameraDevice_close(dev);
        return false;
    }
    AImageReader_ImageListener previewListener { nullptr, OnPreviewImage };
    p_AImageReader_setImageListener(previewReader, &previewListener);
    ANativeWindow *previewWindow = nullptr;
    p_AImageReader_getWindow(previewReader, &previewWindow);
    Log("preview reader %dx%d YUV_420_888, window=%p", opt.width, opt.height, previewWindow);

    // JPEG reader ------------------------------------------------------------
    AImageReader *jpegReader = nullptr;
    ANativeWindow *jpegWindow = nullptr;
    int32_t jw = opt.jpegWidth;
    int32_t jh = opt.jpegHeight;
    if (opt.jpeg) {
        if (jw == 0 || jh == 0) {
            if (!PickJpegSize(mgr, opt.camId, &jw, &jh)) {
                jw = 1920;
                jh = 1080;
            }
        }
        ms = p_AImageReader_new(jw, jh, AIMAGE_FORMAT_JPEG, /*maxImages=*/2, &jpegReader);
        if (ms != AMEDIA_OK) {
            Log("!! AImageReader_new(jpeg %dx%d) failed: %d", jw, jh, ms);
            jpegReader = nullptr;
        } else {
            AImageReader_ImageListener jpegListener { nullptr, OnJpegImage };
            p_AImageReader_setImageListener(jpegReader, &jpegListener);
            p_AImageReader_getWindow(jpegReader, &jpegWindow);
            Log("jpeg reader %dx%d, window=%p", jw, jh, jpegWindow);
        }
    }

    // Session ----------------------------------------------------------------
    ACaptureSessionOutputContainer *outputs = nullptr;
    p_ACaptureSessionOutputContainer_create(&outputs);
    ACaptureSessionOutput *previewOut = nullptr;
    p_ACaptureSessionOutput_create(previewWindow, &previewOut);
    p_ACaptureSessionOutputContainer_add(outputs, previewOut);
    ACaptureSessionOutput *jpegOut = nullptr;
    if (jpegWindow != nullptr) {
        p_ACaptureSessionOutput_create(jpegWindow, &jpegOut);
        p_ACaptureSessionOutputContainer_add(outputs, jpegOut);
    }

    ACameraCaptureSession_stateCallbacks sessCb {};
    sessCb.context = nullptr;
    sessCb.onClosed = OnSessionClosed;
    sessCb.onReady = OnSessionReady;
    sessCb.onActive = OnSessionActive;

    ACameraCaptureSession *session = nullptr;
    t = NowMs();
    st = p_ACameraDevice_createCaptureSession(dev, outputs, &sessCb, &session);
    Log("createCaptureSession -> %s (%.0f ms)", StatusName(st), NowMs() - t);
    if (st != ACAMERA_OK || session == nullptr) {
        Log("*** stream configuration failed — record the container logcat ***");
        p_ACameraDevice_close(dev);
        return false;
    }

    // Repeating preview request ---------------------------------------------
    ACaptureRequest *previewReq = nullptr;
    st = p_ACameraDevice_createCaptureRequest(dev, TEMPLATE_PREVIEW, &previewReq);
    Log("createCaptureRequest(PREVIEW) -> %s", StatusName(st));
    ACameraOutputTarget *previewTarget = nullptr;
    p_ACameraOutputTarget_create(previewWindow, &previewTarget);
    p_ACaptureRequest_addTarget(previewReq, previewTarget);

    ACameraCaptureSession_captureCallbacks capCb {};
    capCb.context = nullptr;
    capCb.onCaptureStarted = OnCaptureStarted;
    capCb.onCaptureProgressed = nullptr;
    capCb.onCaptureCompleted = OnCaptureCompleted;
    capCb.onCaptureFailed = OnCaptureFailed;
    capCb.onCaptureSequenceCompleted = OnSequenceCompleted;
    capCb.onCaptureSequenceAborted = OnSequenceAborted;
    capCb.onCaptureBufferLost = OnBufferLost;

    {
        std::lock_guard<std::mutex> l(g_h.lock);
        g_h.previewFrames = 0;
        g_h.previewDropped = 0;
        g_h.captureStarted = 0;
        g_h.captureCompleted = 0;
        g_h.captureFailed = 0;
        g_h.jpegDone = false;
    }

    t = NowMs();
    st = p_ACameraCaptureSession_setRepeatingRequest(session, &capCb, 1, &previewReq, nullptr);
    Log("setRepeatingRequest -> %s", StatusName(st));

    // Wait for frames --------------------------------------------------------
    {
        std::unique_lock<std::mutex> l(g_h.lock);
        bool ok = g_h.cv.wait_for(l, std::chrono::seconds(15), [&] {
            return g_h.previewFrames >= opt.frames || g_h.deviceError || g_h.deviceDisconnected;
        });
        double span = g_h.lastFrameMs - g_h.firstFrameMs;
        double fps = (g_h.previewFrames > 1 && span > 0)
                         ? (g_h.previewFrames - 1) * 1000.0 / span : 0.0;
        Log("preview: %u frames (%u dropped) in %.0f ms => %.1f fps; firstFrame %.0f ms "
            "after request; timeout=%d",
            g_h.previewFrames, g_h.previewDropped, span, fps, g_h.firstFrameMs - t, !ok);
        Log("capture cbs: started=%u completed=%u failed=%u",
            g_h.captureStarted, g_h.captureCompleted, g_h.captureFailed);
    }

    // Still capture ----------------------------------------------------------
    if (jpegWindow != nullptr) {
        ACaptureRequest *stillReq = nullptr;
        st = p_ACameraDevice_createCaptureRequest(dev, TEMPLATE_STILL_CAPTURE, &stillReq);
        Log("createCaptureRequest(STILL_CAPTURE) -> %s", StatusName(st));
        ACameraOutputTarget *jpegTarget = nullptr;
        p_ACameraOutputTarget_create(jpegWindow, &jpegTarget);
        p_ACaptureRequest_addTarget(stillReq, jpegTarget);
        int32_t orientation = 90;
        p_ACaptureRequest_setEntry_i32(stillReq, ACAMERA_JPEG_ORIENTATION, 1, &orientation);
        uint8_t quality = 95;
        p_ACaptureRequest_setEntry_u8(stillReq, ACAMERA_JPEG_QUALITY, 1, &quality);

        t = NowMs();
        st = p_ACameraCaptureSession_capture(session, &capCb, 1, &stillReq, nullptr);
        Log("capture(STILL) -> %s", StatusName(st));
        {
            std::unique_lock<std::mutex> l(g_h.lock);
            bool ok = g_h.cv.wait_for(l, std::chrono::seconds(20), [&] { return g_h.jpegDone; });
            Log("still capture %s after %.0f ms (%zu bytes)",
                ok ? "DONE" : "TIMED OUT", NowMs() - t, g_h.jpegBytes);
        }
        p_ACameraOutputTarget_free(jpegTarget);
        p_ACaptureRequest_free(stillReq);
    }

    // Soak -------------------------------------------------------------------
    if (opt.soakSec > 0) {
        Log("soaking preview for %d s…", opt.soakSec);
        uint32_t start = 0;
        {
            std::lock_guard<std::mutex> l(g_h.lock);
            start = g_h.previewFrames;
        }
        double t1 = NowMs();
        sleep(static_cast<unsigned>(opt.soakSec));
        std::lock_guard<std::mutex> l(g_h.lock);
        double span = NowMs() - t1;
        Log("soak: %u frames in %.0f ms => %.1f fps (dropped %u, failed %u)",
            g_h.previewFrames - start, span,
            (g_h.previewFrames - start) * 1000.0 / span, g_h.previewDropped, g_h.captureFailed);
    }

    // Teardown ---------------------------------------------------------------
    t = NowMs();
    p_ACameraCaptureSession_stopRepeating(session);
    p_ACameraCaptureSession_close(session);
    p_ACameraOutputTarget_free(previewTarget);
    p_ACaptureRequest_free(previewReq);
    if (jpegOut != nullptr) {
        p_ACaptureSessionOutput_free(jpegOut);
    }
    p_ACaptureSessionOutput_free(previewOut);
    p_ACaptureSessionOutputContainer_free(outputs);
    p_ACameraDevice_close(dev);
    if (jpegReader != nullptr) {
        p_AImageReader_delete(jpegReader);
    }
    p_AImageReader_delete(previewReader);
    Log("teardown done (%.0f ms)", NowMs() - t);
    return true;
}

static void Usage(const char *argv0)
{
    printf(
        "usage: %s [options]\n"
        "  --list                enumerate cameras + characteristics, then exit\n"
        "  --dump-all            also dump every characteristic tag numerically\n"
        "  --cam <id>            camera id to open (default 0)\n"
        "  --w <n> --h <n>       preview size (default 1280x720)\n"
        "  --jpeg-w/--jpeg-h <n> still size (default: largest BLOB <= 4096 wide)\n"
        "  --frames <n>          preview frames to wait for (default 60)\n"
        "  --no-jpeg             skip the still capture\n"
        "  --out <path>          where to write the JPEG (default /data/c0.jpg)\n"
        "  --soak <sec>          keep the preview running afterwards\n"
        "  --reopen <n>          repeat the whole open/stream/close cycle n times\n"
        "  --avail <sec>         register availability callbacks and watch for n s\n"
        "  --uid <n> --gid <n>   drop to this uid/gid before touching anything\n",
        argv0);
}

int main(int argc, char **argv)
{
    g_t0 = NowMs();
    Options opt;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int32_t def) -> int32_t {
            return (i + 1 < argc) ? static_cast<int32_t>(strtol(argv[++i], nullptr, 0)) : def;
        };
        if (a == "--list") {
            opt.listOnly = true;
        } else if (a == "--dump-all") {
            opt.dumpAll = true;
        } else if (a == "--cam" && i + 1 < argc) {
            opt.camId = argv[++i];
        } else if (a == "--w") {
            opt.width = next(opt.width);
        } else if (a == "--h") {
            opt.height = next(opt.height);
        } else if (a == "--jpeg-w") {
            opt.jpegWidth = next(0);
        } else if (a == "--jpeg-h") {
            opt.jpegHeight = next(0);
        } else if (a == "--frames") {
            opt.frames = static_cast<uint32_t>(next(60));
        } else if (a == "--no-jpeg") {
            opt.jpeg = false;
        } else if (a == "--out" && i + 1 < argc) {
            opt.out = argv[++i];
        } else if (a == "--soak") {
            opt.soakSec = next(0);
        } else if (a == "--reopen") {
            opt.reopen = next(1);
        } else if (a == "--avail") {
            opt.availSec = next(0);
        } else if (a == "--no-proxy") {
            opt.proxy = false;
        } else if (a == "--uid") {
            opt.uid = static_cast<uid_t>(next(0));
            opt.dropPriv = true;
        } else if (a == "--gid") {
            opt.gid = static_cast<gid_t>(next(0));
            opt.dropPriv = true;
        } else if (a == "-h" || a == "--help") {
            Usage(argv[0]);
            return 0;
        } else {
            printf("unknown option: %s\n", a.c_str());
            Usage(argv[0]);
            return 1;
        }
    }
    g_h.jpegPath = opt.out;

    if (opt.dropPriv) {
        if (opt.gid != 0 && setgid(opt.gid) != 0) {
            Log("setgid(%u) failed: %s", opt.gid, strerror(errno));
        }
        if (opt.uid != 0 && setuid(opt.uid) != 0) {
            Log("setuid(%u) failed: %s — continuing as uid %u", opt.uid, strerror(errno),
                getuid());
        }
    }
    Log("hybris_camera_test starting: pid=%d uid=%u euid=%u gid=%u", getpid(), getuid(),
        geteuid(), getgid());

    if (!LoadNdk()) {
        Log("*** C0 GATE: the camera2 NDK could not be hosted — see errors above ***");
        return 2;
    }
    Log("NDK entry points resolved");

    // Without this, CameraService's device-policy query fails closed and every
    // connectDevice() comes back ERROR_DISABLED (see the header for why).
    if (opt.proxy) {
        OHOS::HybrisCamera::SetProxyLogger([](const char *msg) { Log("%s", msg); });
        bool ok = OHOS::HybrisCamera::EnsureCameraServiceProxy();
        Log("camera service proxy stub: %s", ok ? "registered" : "FAILED");
    } else {
        Log("camera service proxy stub: skipped (--no-proxy)");
    }

    ACameraManager *mgr = p_ACameraManager_create();
    if (mgr == nullptr) {
        Log("*** C0 GATE: ACameraManager_create returned NULL ***");
        return 3;
    }
    Log("ACameraManager @ %p", mgr);

    ACameraIdList *ids = nullptr;
    double t = NowMs();
    camera_status_t st = p_ACameraManager_getCameraIdList(mgr, &ids);
    Log("getCameraIdList -> %s (%.0f ms)", StatusName(st), NowMs() - t);
    if (st != ACAMERA_OK || ids == nullptr) {
        Log("*** C0 GATE: no camera list — CameraService unreachable or denied ***");
        p_ACameraManager_delete(mgr);
        return 4;
    }
    Log("*** C0: CameraService reachable, %d camera(s) ***", ids->numCameras);
    for (int i = 0; i < ids->numCameras; ++i) {
        Log("  camera[%d] = \"%s\"", i, ids->cameraIds[i]);
    }
    for (int i = 0; i < ids->numCameras; ++i) {
        DumpCharacteristics(mgr, ids->cameraIds[i], opt.dumpAll);
    }
    p_ACameraManager_deleteCameraIdList(ids);

    ACameraManager_AvailabilityCallbacks availCb { nullptr, OnCameraAvailable,
                                                   OnCameraUnavailable };
    if (opt.availSec > 0) {
        st = p_ACameraManager_registerAvailabilityCallback(mgr, &availCb);
        Log("registerAvailabilityCallback -> %s; watching %d s", StatusName(st), opt.availSec);
        sleep(static_cast<unsigned>(opt.availSec));
        p_ACameraManager_unregisterAvailabilityCallback(mgr, &availCb);
    }

    int rc = 0;
    if (!opt.listOnly) {
        for (int i = 0; i < opt.reopen; ++i) {
            if (!RunOnce(mgr, opt, i + 1)) {
                rc = 5;
                break;
            }
        }
    }

    p_ACameraManager_delete(mgr);
    Log("done, rc=%d", rc);
    return rc;
}
