/*
 * Copyright (c) 2026 Oniro Authors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef HYBRIS_AUDIO_COMMON_H
#define HYBRIS_AUDIO_COMMON_H

#include <stdint.h>
#include <string>

extern "C" {
#include <hdf_base.h>
#include "audio_types_vdi.h"
#include "iaudio_manager_vdi.h"
#include "iaudio_adapter_vdi.h"
#include "iaudio_render_vdi.h"
#include "iaudio_capture_vdi.h"
#include "iaudio_callback_vdi.h"

#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <system/audio.h>
#include <cutils/bitops.h>
}

// hilog with our own domain, mirroring the display VDI.  Not stderr (the 13A
// prototype's choice — audio_host's stderr goes nowhere) and not HDF_LOG*,
// which produced no output at all from this module.  A bridge that fails
// silently inside a driver host is not debuggable.
#include "hilog/log.h"
#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "HybrisAudio"
#define LOG_DOMAIN 0xD001401
#define HB_LOGI(fmt, ...) HILOG_INFO(LOG_CORE, "[%{public}s] " fmt, __func__, ##__VA_ARGS__)
#define HB_LOGW(fmt, ...) HILOG_WARN(LOG_CORE, "[%{public}s] " fmt, __func__, ##__VA_ARGS__)
#define HB_LOGE(fmt, ...) HILOG_ERROR(LOG_CORE, "[%{public}s] " fmt, __func__, ##__VA_ARGS__)
#define HB_LOGD(fmt, ...) HILOG_DEBUG(LOG_CORE, "[%{public}s] " fmt, __func__, ##__VA_ARGS__)

// androidd raises this once the container's HAL fleet is up; the audio HAL
// binder-connects to services it starts, so loading it earlier yields a
// half-initialised device. Same signal the display VDIs wait on.
#define ANDROID_READY_PARAM     "android.composer.ready"
#define ANDROID_READY_TIMEOUT_S 90

namespace OHOS::HDI::Audio::Hybris {

// Runtime-loaded hw_get_module (avoids link-time dep on libhardware, which
// isn't permitted for passthrough modules by deps_guard).
int HybrisHwGetModuleByClass(const char *class_id, const char *inst,
    const struct hw_module_t **module);
int HybrisHwGetModule(const char *id, const struct hw_module_t **module);

// Forward decls
struct HybrisAudioAdapter;
struct HybrisAudioRender;
struct HybrisAudioCapture;

// VDI -> Android conversion helpers
audio_format_t VdiFormatToAndroid(enum AudioFormatVdi f);
enum AudioFormatVdi AndroidFormatToVdi(audio_format_t f);

audio_channel_mask_t VdiChannelsToAndroidOut(uint32_t channelCount);
audio_channel_mask_t VdiChannelsToAndroidIn(uint32_t channelCount);

audio_devices_t VdiPinToAndroidOutDevice(enum AudioPortPinVdi pin);
audio_devices_t VdiPinToAndroidInDevice(enum AudioPortPinVdi pin);

audio_source_t VdiInputTypeToAndroidSource(int32_t sourceType);

} // namespace

#endif // HYBRIS_AUDIO_COMMON_H
