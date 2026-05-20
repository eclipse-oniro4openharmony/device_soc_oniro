/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Hand-rolled `CameraAbility` blobs for the three Volla X23 cameras the
 * Halium HAL exposes:
 *
 *   ohosId   vendorId  sensor       role               orientation
 *   lcam001  "0"       S5KGM1ST     rear primary       90
 *   lcam002  "1"       OV16A1Q      front              270
 *   lcam003  "2"       GC08A3WIDE   rear wide          90
 *
 * Sized so the OHOS camera framework (`CameraManager` / `CameraInput`)
 * accepts each camera and is willing to call OpenCamera + configure
 * streams.  Mirrors the key set of `vendor/.../camera_host_config.hcs`
 * ability_01.
 *
 * Long-term this should be replaced by a translation of Halium's
 * `ICameraDevice::getCameraCharacteristics` HIDL metadata, but
 * hand-rolling is the fastest path to a working preview pipeline and
 * keeps N12.5.4 unblocked from N12.5.5+.
 */

#ifndef HYBRIS_CAMERA_ABILITY_H
#define HYBRIS_CAMERA_ABILITY_H

#include <cstdint>
#include <string>
#include <vector>

namespace OHOS::Camera::Hybris {

/*
 * Build the OHOS-format `cameraAbility` byte blob for `vendorId`
 * ("0"/"1"/"2" — the post-slash suffix from the Halium HIDL camera id).
 * Returns false if vendorId is unknown.
 */
bool BuildCameraAbility(const std::string &vendorId,
                        std::vector<uint8_t> &cameraAbility);

} // namespace OHOS::Camera::Hybris

#endif
