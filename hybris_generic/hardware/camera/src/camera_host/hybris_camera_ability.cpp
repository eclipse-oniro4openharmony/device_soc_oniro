/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hybris_camera_ability.h"

#include "camera_device_ability_items.h"
#include "camera_metadata_info.h"
#include "hybris_camera_log.h"
#include "metadata_utils.h"

namespace OHOS::Camera::Hybris {

namespace {

struct CameraProfile {
    const char *vendorId;          // "0" / "1" / "2"
    uint8_t     position;          // camera_position_enum_t
    uint8_t     type;              // camera_type_enum_t
    int32_t     orientation;       // 0/90/180/270
    int32_t     activeArrayW;
    int32_t     activeArrayH;
    int32_t     pixelArrayW;
    int32_t     pixelArrayH;
    int32_t     jpegMaxSize;
    int32_t     jpegW;
    int32_t     jpegH;
    bool        flashAvailable;
    const char *label;             // human-readable for logs
};

/*
 * X23 camera profiles.  Derived from:
 *   - Halium ICameraProvider::getCameraIdList returns 3 devices
 *     (device@3.6/internal/0..2)
 *   - X23 hardware: S5KGM1ST (rear primary, 48MP),
 *     OV16A1Q (front, 16MP), GC08A3WIDE (rear wide, 8MP)
 *   - dumpsys media.camera on Halium reports each sensor's
 *     active-array + orientation (recorded in
 *     phase_n12_camera.md § N12.2.5)
 *
 * Resolutions kept conservative — 720p preview + ~12MP JPEG —
 * to stay safely inside MTK ISP6s bandwidth on first bring-up.
 * Higher resolutions can be enabled per-sensor once the capture
 * loop is proven (N12.6+).
 */
constexpr CameraProfile kProfiles[] = {
    /* vendorId  position                    type                       orientation
       activeArrayW  activeArrayH  pixelArrayW  pixelArrayH
       jpegMaxSize  jpegW  jpegH  flash  label */
    { "0", OHOS_CAMERA_POSITION_BACK,  OHOS_CAMERA_TYPE_WIDE_ANGLE,   90,
      4000, 3000, 4000, 3000,
      16 * 1024 * 1024, 4000, 3000, true,  "S5KGM1ST rear primary" },
    { "1", OHOS_CAMERA_POSITION_FRONT, OHOS_CAMERA_TYPE_WIDE_ANGLE,   270,
      3264, 2448, 3264, 2448,
      12 * 1024 * 1024, 3264, 2448, false, "OV16A1Q front" },
    { "2", OHOS_CAMERA_POSITION_BACK,  OHOS_CAMERA_TYPE_ULTRA_WIDE,   90,
      3264, 2448, 3264, 2448,
      12 * 1024 * 1024, 3264, 2448, true,  "GC08A3WIDE rear wide" },
};

const CameraProfile *FindProfile(const std::string &vendorId)
{
    for (const auto &p : kProfiles) {
        if (vendorId == p.vendorId) {
            return &p;
        }
    }
    return nullptr;
}

/*
 * Triplets are (format, width, height).  Formats are
 * camera_format_t enum values from camera_device_ability_items.h.
 *
 * **Pitfall:** the OHOS camera framework (CameraManager::metaToFwCameraFormat_)
 * does NOT translate `OHOS_CAMERA_FORMAT_YCBCR_420_888`; it gets mapped
 * to `CAMERA_FORMAT_INVALID`, and `createPreviewOutput` then fails with
 * INVALID_ARGUMENT (7400101).  Use `OHOS_CAMERA_FORMAT_YCRCB_420_SP`
 * (NV21, =3) — the framework maps this to `CAMERA_FORMAT_YUV_420_SP`
 * (=1003), which is what the standard OHOS Camera HAP requests for its
 * preview output.
 *
 *   3 = OHOS_CAMERA_FORMAT_YCRCB_420_SP (NV21) → CAMERA_FORMAT_YUV_420_SP
 *   5 = OHOS_CAMERA_FORMAT_JPEG               → CAMERA_FORMAT_JPEG
 *
 * Keep the count modest — the camera framework iterates every entry
 * for stream-config validation.
 */
std::vector<int32_t> MakeBasicConfigurations(const CameraProfile &p)
{
    return {
        OHOS_CAMERA_FORMAT_YCRCB_420_SP,  1280,         720,
        OHOS_CAMERA_FORMAT_YCRCB_420_SP,  640,          480,
        OHOS_CAMERA_FORMAT_JPEG,          p.jpegW,      p.jpegH,
        OHOS_CAMERA_FORMAT_JPEG,          1280,         720,
    };
}

/*
 * Extend configuration encoding (parsed by `CameraStreamInfoParse` in
 * the camera framework — see
 * `camera_stream_info_parse.h::getModeInfo`):
 *
 *   [ modeName,
 *       streamType,
 *           format, width, height, fixedFps, minFps, maxFps,
 *               [abilityId, abilityId, …],   ← optional, can be empty
 *           ABILITY_FINISH (-1),
 *           … more (format, w, h, …) details for this stream …
 *           ABILITY_FINISH (-1),
 *       STREAM_FINISH (-1),
 *       … more streams (streamType + details + STREAM_FINISH) …
 *       STREAM_FINISH (-1),
 *       MODE_FINISH (-1),
 *   ]
 *
 * The mode/stream/ability terminators are critical: the parser uses
 * the triplet `[…, ABILITY_FINISH, STREAM_FINISH, MODE_FINISH]` as the
 * mode boundary.  Getting the structure wrong silently produces an
 * empty profile list (which surfaces as `createPreviewOutput` failing
 * with 7400101 INVALID_ARGUMENT in the camera HAP).
 *
 * Stream types are `OutputCapStreamType` from camera_manager.h:
 *   0 PREVIEW, 1 VIDEO_STREAM, 2 STILL_CAPTURE.
 *
 * Modes used: 0 = NORMAL (SceneMode::NORMAL — preview + still),
 *             1 = CAPTURE (SceneMode::CAPTURE) is the alternate value
 *             camera_manager often parses; the HAP requests NORMAL.
 */
std::vector<int32_t> MakeExtendConfigurations(const CameraProfile &p)
{
    constexpr int32_t kModeNormal     = 0;   /* SceneMode::NORMAL */
    constexpr int32_t kStreamPreview  = 0;
    constexpr int32_t kStreamStill    = 2;
    constexpr int32_t kAbilityFinish  = -1;
    constexpr int32_t kStreamFinish   = -1;
    constexpr int32_t kModeFinish     = -1;

    /*
     * NORMAL mode: a single preview YUV at 1280x720 + 640x480, and
     * the per-sensor max-resolution JPEG plus a downscaled JPEG.
     * fixedFps=0 / minFps=15 / maxFps=30 for the preview frames;
     * still-capture rates are 0/0/0 (capture is one-shot).
     */
    return {
        kModeNormal,
            kStreamPreview,
                OHOS_CAMERA_FORMAT_YCRCB_420_SP, 1280, 720, 0, 15, 30,
                kAbilityFinish,
                OHOS_CAMERA_FORMAT_YCRCB_420_SP, 640,  480, 0, 15, 30,
                kAbilityFinish,
            kStreamFinish,
            kStreamStill,
                OHOS_CAMERA_FORMAT_JPEG, p.jpegW, p.jpegH, 0, 0, 0,
                kAbilityFinish,
                OHOS_CAMERA_FORMAT_JPEG, 1280, 720, 0, 0, 0,
                kAbilityFinish,
            kStreamFinish,
        kModeFinish,
    };
}

template <typename T>
bool Add(const std::shared_ptr<CameraMetadata> &m, uint32_t tag, const T &v)
{
    return m->addEntry(tag, static_cast<const void *>(&v), 1);
}

bool AddArr(const std::shared_ptr<CameraMetadata> &m, uint32_t tag,
            const void *data, size_t count)
{
    return m->addEntry(tag, data, count);
}

void PopulateMetadata(const CameraProfile &p,
                      const std::shared_ptr<CameraMetadata> &meta)
{
    /* ── Camera identity ──────────────────────────────────────────── */
    Add<uint8_t>(meta, OHOS_ABILITY_CAMERA_POSITION,        p.position);
    Add<uint8_t>(meta, OHOS_ABILITY_CAMERA_TYPE,            p.type);
    Add<uint8_t>(meta, OHOS_ABILITY_CAMERA_CONNECTION_TYPE,
                 static_cast<uint8_t>(OHOS_CAMERA_CONNECTION_TYPE_BUILTIN));
    Add<uint8_t>(meta, OHOS_ABILITY_MEMORY_TYPE,
                 static_cast<uint8_t>(OHOS_CAMERA_MEMORY_DMABUF));
    Add<int32_t>(meta, OHOS_SENSOR_ORIENTATION,             p.orientation);

    /* ── Sensor info ─────────────────────────────────────────────── */
    int32_t activeArray[] = {0, 0, p.activeArrayW, p.activeArrayH};
    AddArr(meta, OHOS_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
           activeArray, std::size(activeArray));
    int32_t pixelArray[] = {p.pixelArrayW, p.pixelArrayH};
    AddArr(meta, OHOS_SENSOR_INFO_PIXEL_ARRAY_SIZE,
           pixelArray, std::size(pixelArray));

    /* ── Frame rate ──────────────────────────────────────────────── */
    int32_t fpsRanges[] = {15, 30};
    AddArr(meta, OHOS_ABILITY_FPS_RANGES, fpsRanges, std::size(fpsRanges));

    /* ── 3A / AE / AWB / AF ─────────────────────────────────────── */
    uint8_t aeModes[] = {OHOS_CAMERA_AE_MODE_ON, OHOS_CAMERA_AE_MODE_OFF};
    AddArr(meta, OHOS_CONTROL_AE_AVAILABLE_MODES,
           aeModes, std::size(aeModes));

    uint8_t aeAntiBanding[] = {
        OHOS_CAMERA_AE_ANTIBANDING_MODE_OFF,
        OHOS_CAMERA_AE_ANTIBANDING_MODE_50HZ,
        OHOS_CAMERA_AE_ANTIBANDING_MODE_60HZ,
        OHOS_CAMERA_AE_ANTIBANDING_MODE_AUTO,
    };
    AddArr(meta, OHOS_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
           aeAntiBanding, std::size(aeAntiBanding));

    int32_t aeCompRange[] = {-2, 2};
    AddArr(meta, OHOS_ABILITY_AE_COMPENSATION_RANGE,
           aeCompRange, std::size(aeCompRange));
    int32_t aeCompStepNum   = 1;
    int32_t aeCompStepDen   = 1;
    int32_t aeCompStep[]    = {aeCompStepNum, aeCompStepDen};
    AddArr(meta, OHOS_ABILITY_AE_COMPENSATION_STEP,
           aeCompStep, std::size(aeCompStep));

    uint8_t awbModes[] = {
        OHOS_CAMERA_AWB_MODE_OFF, OHOS_CAMERA_AWB_MODE_AUTO,
    };
    AddArr(meta, OHOS_CONTROL_AWB_AVAILABLE_MODES,
           awbModes, std::size(awbModes));

    uint8_t exposureModes[] = {
        OHOS_CAMERA_EXPOSURE_MODE_AUTO, OHOS_CAMERA_EXPOSURE_MODE_LOCKED,
    };
    AddArr(meta, OHOS_ABILITY_EXPOSURE_MODES,
           exposureModes, std::size(exposureModes));

    uint8_t focusModes[] = {
        OHOS_CAMERA_FOCUS_MODE_AUTO,
        OHOS_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO,
        OHOS_CAMERA_FOCUS_MODE_LOCKED,
    };
    AddArr(meta, OHOS_ABILITY_FOCUS_MODES,
           focusModes, std::size(focusModes));

    /* ── Flash ───────────────────────────────────────────────────── */
    uint8_t flashAvail = p.flashAvailable
        ? static_cast<uint8_t>(OHOS_CAMERA_FLASH_TRUE)
        : static_cast<uint8_t>(OHOS_CAMERA_FLASH_FALSE);
    Add<uint8_t>(meta, OHOS_ABILITY_FLASH_AVAILABLE, flashAvail);
    if (p.flashAvailable) {
        uint8_t flashModes[] = {
            OHOS_CAMERA_FLASH_MODE_CLOSE,
            OHOS_CAMERA_FLASH_MODE_OPEN,
            OHOS_CAMERA_FLASH_MODE_AUTO,
            OHOS_CAMERA_FLASH_MODE_ALWAYS_OPEN,
        };
        AddArr(meta, OHOS_ABILITY_FLASH_MODES,
               flashModes, std::size(flashModes));
    } else {
        uint8_t flashModes[] = { OHOS_CAMERA_FLASH_MODE_CLOSE };
        AddArr(meta, OHOS_ABILITY_FLASH_MODES,
               flashModes, std::size(flashModes));
    }

    /* ── Zoom ────────────────────────────────────────────────────── */
    float zoomRange[] = {1.0f, 4.0f};
    AddArr(meta, OHOS_ABILITY_ZOOM_RATIO_RANGE,
           zoomRange, std::size(zoomRange));

    /* ── Video stabilization (off-only for now) ─────────────────── */
    uint8_t stabModes[] = { OHOS_CAMERA_VIDEO_STABILIZATION_OFF };
    AddArr(meta, OHOS_ABILITY_VIDEO_STABILIZATION_MODES,
           stabModes, std::size(stabModes));

    /* ── JPEG ────────────────────────────────────────────────────── */
    Add<int32_t>(meta, OHOS_JPEG_MAX_SIZE, p.jpegMaxSize);
    int32_t thumbSizes[] = {0, 0, 240, 180, 320, 240};
    AddArr(meta, OHOS_JPEG_AVAILABLE_THUMBNAIL_SIZES,
           thumbSizes, std::size(thumbSizes));

    /* ── Stream configurations ──────────────────────────────────── */
    auto basicCfg  = MakeBasicConfigurations(p);
    AddArr(meta, OHOS_ABILITY_STREAM_AVAILABLE_BASIC_CONFIGURATIONS,
           basicCfg.data(), basicCfg.size());
    auto extendCfg = MakeExtendConfigurations(p);
    AddArr(meta, OHOS_ABILITY_STREAM_AVAILABLE_EXTEND_CONFIGURATIONS,
           extendCfg.data(), extendCfg.size());

    /* ── Capability advertisement to the framework ─────────────── */
    int32_t fpsRange1530[] = {15, 30};
    AddArr(meta, OHOS_CONTROL_AE_TARGET_FPS_RANGE,
           fpsRange1530, std::size(fpsRange1530));
}

} // namespace

bool BuildCameraAbility(const std::string &vendorId,
                        std::vector<uint8_t> &cameraAbility)
{
    const auto *profile = FindProfile(vendorId);
    if (profile == nullptr) {
        CAMERA_VDI_LOGW("BuildCameraAbility: unknown vendorId=%{public}s",
                        vendorId.c_str());
        return false;
    }

    /* Capacity sized to comfortably hold the populated entries above
     * (~25 items, ~500 bytes data).  Over-allocation is cheap; resize
     * triggers an internal realloc which we'd rather avoid mid-build. */
    constexpr size_t kItemCapacity = 64;
    constexpr size_t kDataCapacity = 2048;
    auto meta = std::make_shared<CameraMetadata>(kItemCapacity, kDataCapacity);
    if (meta == nullptr || meta->get() == nullptr) {
        CAMERA_VDI_LOGE("BuildCameraAbility: CameraMetadata alloc failed");
        return false;
    }

    PopulateMetadata(*profile, meta);

    if (!MetadataUtils::ConvertMetadataToVec(meta, cameraAbility)) {
        CAMERA_VDI_LOGE("BuildCameraAbility: ConvertMetadataToVec failed");
        return false;
    }

    CAMERA_VDI_LOGI("BuildCameraAbility(%{public}s: %{public}s): "
                    "%{public}zu bytes",
                    vendorId.c_str(), profile->label, cameraAbility.size());
    return true;
}

} // namespace OHOS::Camera::Hybris
