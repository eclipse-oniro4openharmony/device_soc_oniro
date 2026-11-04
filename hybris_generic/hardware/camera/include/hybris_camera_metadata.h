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

#ifndef HYBRIS_CAMERA_METADATA_H
#define HYBRIS_CAMERA_METADATA_H

#include <memory>
#include <string>
#include <vector>

#include "camera_metadata_info.h"

#include "hybris_ndk_api.h"

namespace OHOS {
namespace HybrisCamera {

// A size the HAL advertises for one of the formats we actually serve.
struct StreamSize {
    int32_t width;
    int32_t height;
};

/*
 * One Android camera serving an OHOS camera.  The OHOS model has a single
 * logical camera per position whose lenses are chosen by the HAL: an
 * ultra-wide lens when the zoom ratio drops below 1, a macro lens while the
 * session enables macro.  Each lens is its own Android camera here.
 */
struct LensInfo {
    std::string androidId;
    std::string role;                     // "main", "ultra-wide", "macro"
    int32_t activeArray[4] = { 0, 0, 0, 0 };
    std::vector<StreamSize> yuvSizes;
    std::vector<StreamSize> jpegSizes;
    float zoomMax = 1.0f;                 // the lens's own zoom ratio range max
    float scale = 1.0f;                   // its field of view as a logical zoom ratio
};

// Everything the bridge needs to know about one OHOS camera, derived once
// from ACameraManager_getCameraCharacteristics at enumeration time.
struct CameraInfo {
    std::string vdiId;                    // hybris_back / hybris_front
    std::string androidId;                // "0" / "1" — what the NDK wants
    uint8_t position = 0;                 // OHOS_CAMERA_POSITION_*
    int32_t sensorOrientation = 0;
    bool flashAvailable = false;
    int32_t activeArray[4] = { 0, 0, 0, 0 };
    std::vector<StreamSize> yuvSizes;     // preview + video (NV21)
    std::vector<StreamSize> jpegSizes;    // still capture
    float zoomMax = 1.0f;
    std::shared_ptr<OHOS::Camera::CameraMetadata> ability;
    // lenses[0] is this camera's own sensor; the rest are auxiliary lenses
    // attached with AttachLens.  ultraWide / macro index into it (-1: none).
    std::vector<LensInfo> lenses;
    int ultraWide = -1;
    int macro = -1;
};

// The lens a camera's own characteristics describe (role "main").
LensInfo MainLens(const CameraInfo &info);

/*
 * Makes `aux` (another Android camera, already run through
 * BuildCameraAbility) an auxiliary lens of `info`, and advertises it in
 * info's ability: an ultra-wide lens lowers the zoom ratio range to `scale`,
 * a macro lens sets OHOS_ABILITY_CAMERA_MACRO_SUPPORTED.
 */
bool AttachLens(CameraInfo &info, const CameraInfo &aux, const std::string &role, float scale);

/*
 * Translates Android characteristics into an OHOS camera ability.
 *
 * The two metadata containers share a design (section<<16 tags, aligned
 * entry/data areas) but not a tag space — OHOS renumbered the sections and
 * inserted META_TYPE_UINT32, so every value goes through an explicit mapping
 * rather than a memcpy.  Stream configurations differ structurally too:
 * Android publishes (format, w, h, direction) quads, OHOS
 * OHOS_ABILITY_STREAM_AVAILABLE_BASIC_CONFIGURATIONS (format, w, h) triples.
 */
bool BuildCameraAbility(const NdkApi *ndk, const ACameraMetadata *chars, CameraInfo &info);

/*
 * Applies the OHOS control subset we honour onto an Android capture request.
 * Unknown/unsupported tags are ignored rather than failing the request.
 */
void ApplySettingsToRequest(const NdkApi *ndk, const std::vector<uint8_t> &ohosSettings,
                            const CameraInfo &info, const LensInfo &lens, ACaptureRequest *request);

// What the settings ask of the lens choice: the logical zoom ratio (1 when
// unset) and whether macro is enabled.  Only tags present are written.
void ReadLensControls(const std::vector<uint8_t> &ohosSettings, float &zoom, bool &macro);

// Merges `update` into `merged` (both encoded camera metadata): the framework
// only sends the tags that changed since its last UpdateSettings.
void MergeSettings(std::vector<uint8_t> &merged, const std::vector<uint8_t> &update);

/*
 * Builds the per-frame OHOS result metadata from an Android capture result,
 * limited to the tags the framework has asked for via EnableResult.
 */
std::shared_ptr<OHOS::Camera::CameraMetadata> BuildResultMetadata(
    const NdkApi *ndk, const ACameraMetadata *androidResult, const std::vector<int32_t> &enabled,
    float zoomScale = 1.0f);

// Tags we can populate in OnResult; the device VDI reports these from
// GetEnabledResults and the framework picks from them.
const std::vector<int32_t> &SupportedResultTags();

} // namespace HybrisCamera
} // namespace OHOS

#endif // HYBRIS_CAMERA_METADATA_H
