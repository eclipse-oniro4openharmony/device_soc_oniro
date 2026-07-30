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

#include "hybris_camera_metadata.h"

#include <algorithm>
#include <cmath>

#include "camera_device_ability_items.h"
#include "camera_metadata_operator.h"
#include "metadata_utils.h"

#include "hybris_camera_common.h"

namespace OHOS {
namespace HybrisCamera {
namespace {

using OHOS::Camera::CameraMetadata;

// ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS is *not* published in HAL
// pixel formats: ACameraMetadata rewrites the HAL's list into NDK image
// formats before handing it over, so BLOB shows up as AIMAGE_FORMAT_JPEG
// (0x100) and IMPLEMENTATION_DEFINED as AIMAGE_FORMAT_PRIVATE.
constexpr int32_t NDK_FORMAT_JPEG = AIMAGE_FORMAT_JPEG;
constexpr int32_t NDK_FORMAT_YUV_420_888 = AIMAGE_FORMAT_YUV_420_888;
constexpr int32_t STREAM_DIRECTION_OUTPUT = 0;

// Bring-up caps (plan §D4/§D6): preview/video is CPU-copied per frame, so a
// 4624x3472 YUV stream is not something we want the framework to offer yet,
// and 64 MP stills are not a bring-up feature.
constexpr int32_t MAX_YUV_WIDTH = 1920;
constexpr int32_t MAX_YUV_HEIGHT = 1088;
constexpr int32_t MAX_JPEG_WIDTH = 4096;
constexpr int32_t MIN_DIMENSION = 160;

constexpr size_t ABILITY_ITEM_CAPACITY = 64;
constexpr size_t ABILITY_DATA_CAPACITY = 8192;
constexpr size_t RESULT_ITEM_CAPACITY = 24;
constexpr size_t RESULT_DATA_CAPACITY = 512;

bool GetEntry(const NdkApi *ndk, const ACameraMetadata *meta, uint32_t tag,
              ACameraMetadata_const_entry &entry)
{
    return ndk->ACameraMetadata_getConstEntry(meta, tag, &entry) == ACAMERA_OK && entry.count > 0;
}

// Collect the output sizes the HAL offers for one HAL pixel format, largest
// first, dropping anything outside the bring-up caps.
std::vector<StreamSize> CollectSizes(const NdkApi *ndk, const ACameraMetadata *chars, int32_t format,
                                     int32_t maxWidth, int32_t maxHeight)
{
    std::vector<StreamSize> sizes;
    ACameraMetadata_const_entry entry {};
    if (!GetEntry(ndk, chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, entry)) {
        return sizes;
    }
    constexpr uint32_t QUAD = 4;
    for (uint32_t i = 0; i + QUAD - 1 < entry.count; i += QUAD) {
        if (entry.data.i32[i] != format || entry.data.i32[i + 3] != STREAM_DIRECTION_OUTPUT) {
            continue;
        }
        int32_t w = entry.data.i32[i + 1];
        int32_t h = entry.data.i32[i + 2];
        if (w < MIN_DIMENSION || h < MIN_DIMENSION || w > maxWidth || h > maxHeight) {
            continue;
        }
        sizes.push_back({ w, h });
    }
    std::sort(sizes.begin(), sizes.end(), [](const StreamSize &a, const StreamSize &b) {
        return static_cast<int64_t>(a.width) * a.height > static_cast<int64_t>(b.width) * b.height;
    });
    return sizes;
}

void AddU8(const std::shared_ptr<CameraMetadata> &meta, uint32_t tag, uint8_t value)
{
    meta->addEntry(tag, &value, 1);
}

void AddU8Vec(const std::shared_ptr<CameraMetadata> &meta, uint32_t tag,
              const std::vector<uint8_t> &values)
{
    if (!values.empty()) {
        meta->addEntry(tag, values.data(), values.size());
    }
}

void AddI32Vec(const std::shared_ptr<CameraMetadata> &meta, uint32_t tag,
               const std::vector<int32_t> &values)
{
    if (!values.empty()) {
        meta->addEntry(tag, values.data(), values.size());
    }
}

// Android AF modes -> OHOS focus modes.  OHOS models "continuous" and
// "one-shot" separately and treats LOCKED as a mode rather than a trigger.
std::vector<uint8_t> MapFocusModes(const NdkApi *ndk, const ACameraMetadata *chars)
{
    std::vector<uint8_t> modes;
    ACameraMetadata_const_entry entry {};
    bool hasAuto = false;
    bool hasContinuous = false;
    bool hasOff = false;
    if (GetEntry(ndk, chars, ACAMERA_CONTROL_AF_AVAILABLE_MODES, entry)) {
        for (uint32_t i = 0; i < entry.count; ++i) {
            switch (entry.data.u8[i]) {
                case ACAMERA_CONTROL_AF_MODE_OFF: hasOff = true; break;
                case ACAMERA_CONTROL_AF_MODE_AUTO:
                case ACAMERA_CONTROL_AF_MODE_MACRO: hasAuto = true; break;
                case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE: hasContinuous = true; break;
                default: break;
            }
        }
    }
    if (hasOff) {
        modes.push_back(OHOS_CAMERA_FOCUS_MODE_MANUAL);
    }
    if (hasContinuous) {
        modes.push_back(OHOS_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO);
    }
    if (hasAuto) {
        modes.push_back(OHOS_CAMERA_FOCUS_MODE_AUTO);
        modes.push_back(OHOS_CAMERA_FOCUS_MODE_LOCKED);
    }
    if (modes.empty()) {
        modes.push_back(OHOS_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO);
    }
    return modes;
}

std::vector<uint8_t> MapFlashModes(bool flashAvailable)
{
    if (!flashAvailable) {
        return { OHOS_CAMERA_FLASH_MODE_CLOSE };
    }
    return { OHOS_CAMERA_FLASH_MODE_CLOSE, OHOS_CAMERA_FLASH_MODE_OPEN,
             OHOS_CAMERA_FLASH_MODE_AUTO, OHOS_CAMERA_FLASH_MODE_ALWAYS_OPEN };
}

void AddStreamConfigurations(const std::shared_ptr<CameraMetadata> &meta, const CameraInfo &info)
{
    // OHOS_ABILITY_STREAM_AVAILABLE_BASIC_CONFIGURATIONS is a flat list of
    // (format, width, height) triples.  CameraManager::ParseBasicCapability
    // routes JPEG entries to photo profiles and everything else to preview
    // profiles (and, crossed with OHOS_ABILITY_FPS_RANGES, to video profiles).
    std::vector<int32_t> configs;
    configs.reserve((info.yuvSizes.size() + info.jpegSizes.size()) * 3);
    for (const auto &s : info.yuvSizes) {
        configs.push_back(OHOS_CAMERA_FORMAT_YCRCB_420_SP);
        configs.push_back(s.width);
        configs.push_back(s.height);
    }
    for (const auto &s : info.jpegSizes) {
        configs.push_back(OHOS_CAMERA_FORMAT_JPEG);
        configs.push_back(s.width);
        configs.push_back(s.height);
    }
    AddI32Vec(meta, OHOS_ABILITY_STREAM_AVAILABLE_BASIC_CONFIGURATIONS, configs);
}

} // namespace

bool BuildCameraAbility(const NdkApi *ndk, const ACameraMetadata *chars, CameraInfo &info)
{
    if (ndk == nullptr || chars == nullptr) {
        return false;
    }
    ACameraMetadata_const_entry entry {};

    // ── Placement and orientation ───────────────────────────────────────────
    info.position = OHOS_CAMERA_POSITION_OTHER;
    if (GetEntry(ndk, chars, ACAMERA_LENS_FACING, entry)) {
        info.position = (entry.data.u8[0] == ACAMERA_LENS_FACING_FRONT) ? OHOS_CAMERA_POSITION_FRONT
                                                                       : OHOS_CAMERA_POSITION_BACK;
    }
    if (GetEntry(ndk, chars, ACAMERA_SENSOR_ORIENTATION, entry)) {
        info.sensorOrientation = entry.data.i32[0];
    }
    if (GetEntry(ndk, chars, ACAMERA_FLASH_INFO_AVAILABLE, entry)) {
        info.flashAvailable = entry.data.u8[0] == ACAMERA_FLASH_INFO_AVAILABLE_TRUE;
    }
    if (GetEntry(ndk, chars, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, entry) && entry.count >= 4) {
        for (int i = 0; i < 4; ++i) {
            info.activeArray[i] = entry.data.i32[i];
        }
    }

    // ── Stream configurations ───────────────────────────────────────────────
    info.yuvSizes = CollectSizes(ndk, chars, NDK_FORMAT_YUV_420_888, MAX_YUV_WIDTH, MAX_YUV_HEIGHT);
    info.jpegSizes = CollectSizes(ndk, chars, NDK_FORMAT_JPEG, MAX_JPEG_WIDTH, MAX_JPEG_WIDTH);
    if (info.yuvSizes.empty() || info.jpegSizes.empty()) {
        HC_LOGE("camera %{public}s advertises no usable sizes (yuv=%{public}zu jpeg=%{public}zu)",
                info.androidId.c_str(), info.yuvSizes.size(), info.jpegSizes.size());
        return false;
    }

    auto ability = std::make_shared<CameraMetadata>(ABILITY_ITEM_CAPACITY, ABILITY_DATA_CAPACITY);

    AddU8(ability, OHOS_ABILITY_CAMERA_POSITION, info.position);
    AddU8(ability, OHOS_ABILITY_CAMERA_TYPE, OHOS_CAMERA_TYPE_WIDE_ANGLE);
    AddU8(ability, OHOS_ABILITY_CAMERA_CONNECTION_TYPE, OHOS_CAMERA_CONNECTION_TYPE_BUILTIN);
    // Frames reach us as gralloc buffers from the Android side and are copied
    // into the OHOS stream's own dma-buf backed SurfaceBuffers.
    AddU8(ability, OHOS_ABILITY_MEMORY_TYPE, OHOS_CAMERA_MEMORY_DMABUF);
    AddU8(ability, OHOS_ABILITY_FLASH_AVAILABLE,
          info.flashAvailable ? OHOS_CAMERA_FLASH_TRUE : OHOS_CAMERA_FLASH_FALSE);
    ability->addEntry(OHOS_SENSOR_ORIENTATION, &info.sensorOrientation, 1);
    AddI32Vec(ability, OHOS_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
              { info.activeArray[0], info.activeArray[1], info.activeArray[2], info.activeArray[3] });

    // ── fps ranges ──────────────────────────────────────────────────────────
    std::vector<int32_t> fpsRanges;
    if (GetEntry(ndk, chars, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, entry)) {
        for (uint32_t i = 0; i + 1 < entry.count; i += 2) {
            int32_t lo = entry.data.i32[i];
            int32_t hi = entry.data.i32[i + 1];
            // The framework treats these as [min,max] pairs; drop the
            // high-speed entries we do not configure a session for.
            if (hi > 60) {
                continue;
            }
            fpsRanges.push_back(lo);
            fpsRanges.push_back(hi);
        }
    }
    if (fpsRanges.empty()) {
        fpsRanges = { 15, 30 };
    }
    AddI32Vec(ability, OHOS_ABILITY_FPS_RANGES, fpsRanges);

    AddStreamConfigurations(ability, info);

    // ── 3A ──────────────────────────────────────────────────────────────────
    AddU8Vec(ability, OHOS_ABILITY_FOCUS_MODES, MapFocusModes(ndk, chars));
    AddU8Vec(ability, OHOS_ABILITY_EXPOSURE_MODES,
             { OHOS_CAMERA_EXPOSURE_MODE_CONTINUOUS_AUTO, OHOS_CAMERA_EXPOSURE_MODE_LOCKED });
    AddU8Vec(ability, OHOS_ABILITY_FLASH_MODES, MapFlashModes(info.flashAvailable));

    std::vector<int32_t> aeRange = { 0, 0 };
    if (GetEntry(ndk, chars, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, entry) && entry.count >= 2) {
        aeRange = { entry.data.i32[0], entry.data.i32[1] };
    }
    AddI32Vec(ability, OHOS_ABILITY_AE_COMPENSATION_RANGE, aeRange);
    camera_rational_t aeStep = { 1, 3 };
    if (GetEntry(ndk, chars, ACAMERA_CONTROL_AE_COMPENSATION_STEP, entry)) {
        aeStep.numerator = entry.data.r[0].numerator;
        aeStep.denominator = entry.data.r[0].denominator;
    }
    ability->addEntry(OHOS_ABILITY_AE_COMPENSATION_STEP, &aeStep, 1);

    // ── zoom ────────────────────────────────────────────────────────────────
    std::vector<float> zoomRange = { 1.0f, 1.0f };
    if (GetEntry(ndk, chars, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, entry) && entry.count >= 2) {
        zoomRange = { entry.data.f[0], entry.data.f[1] };
    } else if (GetEntry(ndk, chars, ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, entry)) {
        zoomRange = { 1.0f, entry.data.f[0] };
    }
    ability->addEntry(OHOS_ABILITY_ZOOM_RATIO_RANGE, zoomRange.data(), zoomRange.size());

    AddU8Vec(ability, OHOS_ABILITY_VIDEO_STABILIZATION_MODES, { OHOS_CAMERA_VIDEO_STABILIZATION_OFF });
    AddU8Vec(ability, OHOS_ABILITY_MUTE_MODES, { OHOS_CAMERA_MUTE_MODE_OFF });

    if (!ability->isValid()) {
        HC_LOGE("built ability for %{public}s is not valid", info.vdiId.c_str());
        return false;
    }
    info.ability = ability;
    HC_LOGI("camera %{public}s (android \"%{public}s\"): position=%{public}u orientation=%{public}d "
            "flash=%{public}d yuv=%{public}zu jpeg=%{public}zu largestYuv=%{public}dx%{public}d "
            "largestJpeg=%{public}dx%{public}d",
            info.vdiId.c_str(), info.androidId.c_str(), info.position, info.sensorOrientation,
            info.flashAvailable ? 1 : 0, info.yuvSizes.size(), info.jpegSizes.size(),
            info.yuvSizes[0].width, info.yuvSizes[0].height, info.jpegSizes[0].width,
            info.jpegSizes[0].height);
    return true;
}

void ApplySettingsToRequest(const NdkApi *ndk, const std::vector<uint8_t> &ohosSettings,
                            const CameraInfo &info, ACaptureRequest *request)
{
    if (ndk == nullptr || request == nullptr || ohosSettings.empty()) {
        return;
    }
    std::shared_ptr<CameraMetadata> settings;
    OHOS::Camera::MetadataUtils::ConvertVecToMetadata(ohosSettings, settings);
    if (settings == nullptr || settings->get() == nullptr) {
        return;
    }
    common_metadata_header_t *header = settings->get();
    camera_metadata_item_t item {};

    if (OHOS::Camera::FindCameraMetadataItem(header, OHOS_CONTROL_FOCUS_MODE, &item) == CAM_META_SUCCESS) {
        uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        switch (item.data.u8[0]) {
            case OHOS_CAMERA_FOCUS_MODE_MANUAL: afMode = ACAMERA_CONTROL_AF_MODE_OFF; break;
            case OHOS_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO:
                afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
                break;
            case OHOS_CAMERA_FOCUS_MODE_AUTO:
            case OHOS_CAMERA_FOCUS_MODE_LOCKED: afMode = ACAMERA_CONTROL_AF_MODE_AUTO; break;
            default: break;
        }
        ndk->ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
    }

    if (OHOS::Camera::FindCameraMetadataItem(header, OHOS_CONTROL_FLASH_MODE, &item) == CAM_META_SUCCESS) {
        uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;
        uint8_t flashMode = ACAMERA_FLASH_MODE_OFF;
        switch (item.data.u8[0]) {
            case OHOS_CAMERA_FLASH_MODE_OPEN:
                aeMode = ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH;
                break;
            case OHOS_CAMERA_FLASH_MODE_AUTO:
                aeMode = ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH;
                break;
            case OHOS_CAMERA_FLASH_MODE_ALWAYS_OPEN:
                flashMode = ACAMERA_FLASH_MODE_TORCH;
                break;
            default: break;
        }
        ndk->ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
        ndk->ACaptureRequest_setEntry_u8(request, ACAMERA_FLASH_MODE, 1, &flashMode);
    }

    if (OHOS::Camera::FindCameraMetadataItem(header, OHOS_CONTROL_ZOOM_RATIO, &item) == CAM_META_SUCCESS) {
        float zoom = item.data.f[0];
        ndk->ACaptureRequest_setEntry_float(request, ACAMERA_CONTROL_ZOOM_RATIO, 1, &zoom);
    }

    if (OHOS::Camera::FindCameraMetadataItem(header, OHOS_CONTROL_AE_EXPOSURE_COMPENSATION, &item) ==
        CAM_META_SUCCESS) {
        int32_t comp = item.data.i32[0];
        ndk->ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &comp);
    }

    if (OHOS::Camera::FindCameraMetadataItem(header, OHOS_CONTROL_FPS_RANGES, &item) == CAM_META_SUCCESS &&
        item.count >= 2) {
        int32_t fps[2] = { item.data.i32[0], item.data.i32[1] };
        ndk->ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps);
    }

    if (OHOS::Camera::FindCameraMetadataItem(header, OHOS_JPEG_ORIENTATION, &item) == CAM_META_SUCCESS) {
        int32_t orientation = item.data.i32[0];
        HC_LOGI("jpeg orientation %{public}d", orientation);
        ndk->ACaptureRequest_setEntry_i32(request, ACAMERA_JPEG_ORIENTATION, 1, &orientation);
    }
    if (OHOS::Camera::FindCameraMetadataItem(header, OHOS_JPEG_QUALITY, &item) == CAM_META_SUCCESS) {
        uint8_t quality = static_cast<uint8_t>(item.data.u8[0]);
        ndk->ACaptureRequest_setEntry_u8(request, ACAMERA_JPEG_QUALITY, 1, &quality);
    }

    // AF regions arrive in active-array coordinates on both sides, so the
    // rectangle passes straight through; Android additionally wants a weight.
    if (OHOS::Camera::FindCameraMetadataItem(header, OHOS_CONTROL_AF_REGIONS, &item) == CAM_META_SUCCESS &&
        item.count >= 4) {
        int32_t region[5] = { item.data.i32[0], item.data.i32[1], item.data.i32[2], item.data.i32[3],
                              1000 };
        ndk->ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AF_REGIONS, 5, region);
        ndk->ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_REGIONS, 5, region);
    }
}

const std::vector<int32_t> &SupportedResultTags()
{
    static const std::vector<int32_t> tags = {
        OHOS_CONTROL_FOCUS_MODE,
        OHOS_CONTROL_FOCUS_STATE,
        OHOS_CONTROL_EXPOSURE_STATE,
        OHOS_SENSOR_EXPOSURE_TIME,
        OHOS_CONTROL_FLASH_STATE,
        OHOS_CONTROL_ZOOM_RATIO,
    };
    return tags;
}

std::shared_ptr<OHOS::Camera::CameraMetadata> BuildResultMetadata(
    const NdkApi *ndk, const ACameraMetadata *androidResult, const std::vector<int32_t> &enabled)
{
    if (ndk == nullptr || androidResult == nullptr) {
        return nullptr;
    }
    auto result = std::make_shared<CameraMetadata>(RESULT_ITEM_CAPACITY, RESULT_DATA_CAPACITY);
    auto wants = [&enabled](int32_t tag) {
        return enabled.empty() ||
               std::find(enabled.begin(), enabled.end(), tag) != enabled.end();
    };
    ACameraMetadata_const_entry entry {};

    /*
     * CaptureSession::ProcessAutoFocusUpdates reads the focus MODE out of every
     * result before it will look at the focus state, and logs an error per frame
     * when it is missing -- so leaving it out both floods the log at frame rate
     * and suppresses the app's focus-state callback.  Mirrors MapFocusModes.
     */
    if (wants(OHOS_CONTROL_FOCUS_MODE) &&
        GetEntry(ndk, androidResult, ACAMERA_CONTROL_AF_MODE, entry)) {
        uint8_t mode = OHOS_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO;
        switch (entry.data.u8[0]) {
            case ACAMERA_CONTROL_AF_MODE_OFF:
                mode = OHOS_CAMERA_FOCUS_MODE_MANUAL;
                break;
            case ACAMERA_CONTROL_AF_MODE_AUTO:
            case ACAMERA_CONTROL_AF_MODE_MACRO:
                mode = OHOS_CAMERA_FOCUS_MODE_AUTO;
                break;
            default: // CONTINUOUS_VIDEO / CONTINUOUS_PICTURE / EDOF
                break;
        }
        result->addEntry(OHOS_CONTROL_FOCUS_MODE, &mode, 1);
    }

    if (wants(OHOS_CONTROL_FOCUS_STATE) &&
        GetEntry(ndk, androidResult, ACAMERA_CONTROL_AF_STATE, entry)) {
        // OHOS: SCAN=0, FOCUSED=1, UNFOCUSED=2.
        uint8_t state = OHOS_CAMERA_FOCUS_STATE_SCAN;
        switch (entry.data.u8[0]) {
            case ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED:
            case ACAMERA_CONTROL_AF_STATE_PASSIVE_FOCUSED:
                state = OHOS_CAMERA_FOCUS_STATE_FOCUSED;
                break;
            case ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED:
            case ACAMERA_CONTROL_AF_STATE_PASSIVE_UNFOCUSED:
                state = OHOS_CAMERA_FOCUS_STATE_UNFOCUSED;
                break;
            default: break;
        }
        result->addEntry(OHOS_CONTROL_FOCUS_STATE, &state, 1);
    }

    if (wants(OHOS_CONTROL_EXPOSURE_STATE) &&
        GetEntry(ndk, androidResult, ACAMERA_CONTROL_AE_STATE, entry)) {
        uint8_t state = (entry.data.u8[0] == ACAMERA_CONTROL_AE_STATE_CONVERGED ||
                         entry.data.u8[0] == ACAMERA_CONTROL_AE_STATE_LOCKED)
                            ? OHOS_CAMERA_EXPOSURE_STATE_CONVERGED
                            : OHOS_CAMERA_EXPOSURE_STATE_SCAN;
        result->addEntry(OHOS_CONTROL_EXPOSURE_STATE, &state, 1);
    }

    if (wants(OHOS_SENSOR_EXPOSURE_TIME) &&
        GetEntry(ndk, androidResult, ACAMERA_SENSOR_EXPOSURE_TIME, entry)) {
        int64_t exposure = entry.data.i64[0];
        result->addEntry(OHOS_SENSOR_EXPOSURE_TIME, &exposure, 1);
    }

    if (wants(OHOS_CONTROL_ZOOM_RATIO) &&
        GetEntry(ndk, androidResult, ACAMERA_CONTROL_ZOOM_RATIO, entry)) {
        float zoom = entry.data.f[0];
        result->addEntry(OHOS_CONTROL_ZOOM_RATIO, &zoom, 1);
    }

    if (wants(OHOS_CONTROL_FLASH_STATE) &&
        GetEntry(ndk, androidResult, ACAMERA_FLASH_STATE, entry)) {
        uint8_t state = OHOS_CAMERA_FLASH_STATE_UNKNOWN;
        switch (entry.data.u8[0]) {
            case ACAMERA_FLASH_STATE_UNAVAILABLE: state = OHOS_CAMERA_FLASH_STATE_UNAVAILABLE; break;
            case ACAMERA_FLASH_STATE_CHARGING: state = OHOS_CAMERA_FLASH_STATE_CHARGING; break;
            case ACAMERA_FLASH_STATE_READY: state = OHOS_CAMERA_FLASH_STATE_READY; break;
            case ACAMERA_FLASH_STATE_FIRED:
            case ACAMERA_FLASH_STATE_PARTIAL: state = OHOS_CAMERA_FLASH_STATE_FLASHING; break;
            default: break;
        }
        result->addEntry(OHOS_CONTROL_FLASH_STATE, &state, 1);
    }

    return result;
}

} // namespace HybrisCamera
} // namespace OHOS
