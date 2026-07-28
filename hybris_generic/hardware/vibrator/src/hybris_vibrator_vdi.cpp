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
 *
 * Vibrator VDI for hybris_generic: drives the LED-class vibrator node the
 * MTK/AWinic haptic drivers expose (/sys/class/leds/vibrator — aw-haptic-hv
 * on the Volla Phone Plinius). No HDF kernel driver is involved: a millisecond
 * count goes to `duration` and `activate` starts/stops the motor.
 */

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <unistd.h>
#include <hdf_base.h>
#include <hdf_log.h>
#include "ivibrator_interface_vdi.h"

#define HDF_LOG_TAG hybris_vibrator_vdi

namespace OHOS {
namespace HDI {
namespace Vibrator {
namespace V1_1 {
namespace {

constexpr const char *VIB_DIR = "/sys/class/leds/vibrator";

/* Preset effects: this motor has no waveform library, so every supported
 * effect maps to a plain buzz of a plausible length. Unknown names must
 * fail with HDF_ERR_INVALID_PARAM (the framework and HATS rely on it). */
const std::map<std::string, uint32_t> g_effectDurationMs = {
    { "haptic.default.effect", 40 },
    { "haptic.clock.timer", 30 },
    { "haptic.long_press.light", 40 },
    { "haptic.long_press.medium", 50 },
    { "haptic.long_press.heavy", 60 },
    { "haptic.fail", 60 },
    { "haptic.charging", 50 },
    { "haptic.slide.light", 20 },
    { "haptic.threshold", 30 },
};

int32_t WriteSysfs(const char *file, const std::string &value)
{
    char path[128];
    if (snprintf(path, sizeof(path), "%s/%s", VIB_DIR, file) < 0) {
        return HDF_FAILURE;
    }
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        HDF_LOGE("%{public}s: open %{public}s failed: %{public}d", __func__, path, errno);
        return HDF_FAILURE;
    }
    ssize_t n = write(fd, value.c_str(), value.size());
    close(fd);
    if (n != static_cast<ssize_t>(value.size())) {
        HDF_LOGE("%{public}s: write %{public}s failed: %{public}d", __func__, path, errno);
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

} // namespace

class HybrisVibratorVdi : public IVibratorInterfaceVdi {
public:
    int32_t Init() override
    {
        char path[128];
        if (snprintf(path, sizeof(path), "%s/activate", VIB_DIR) < 0) {
            return HDF_FAILURE;
        }
        if (access(path, W_OK) != 0) {
            HDF_LOGE("%{public}s: no writable vibrator node at %{public}s (%{public}d)",
                __func__, path, errno);
            return HDF_FAILURE;
        }
        HDF_LOGI("%{public}s: using %{public}s", __func__, VIB_DIR);
        return HDF_SUCCESS;
    }

    int32_t StartOnce(uint32_t duration) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return BuzzLocked(duration);
    }

    int32_t Start(const std::string &effectType) override
    {
        auto it = g_effectDurationMs.find(effectType);
        if (it == g_effectDurationMs.end()) {
            HDF_LOGW("%{public}s: unknown effect [%{public}s]", __func__, effectType.c_str());
            return HDF_ERR_INVALID_PARAM;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return BuzzLocked(it->second);
    }

    int32_t Stop(HdfVibratorModeVdi mode) override
    {
        if (mode >= VDI_VIBRATOR_MODE_BUTT) {
            return HDF_ERR_INVALID_PARAM;
        }
        if (mode == VDI_VIBRATOR_MODE_HDHAPTIC) {
            return HDF_ERR_NOT_SUPPORT; /* no HD-haptic waveform engine on this motor */
        }
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        return WriteSysfs("activate", "0");
    }

    int32_t GetVibratorInfo(std::vector<HdfVibratorInfoVdi> &vibratorInfo) override
    {
        HdfVibratorInfoVdi info = {};
        info.isSupportIntensity = false;
        info.isSupportFrequency = false;
        info.intensityMaxValue = 0;
        info.intensityMinValue = 0;
        info.frequencyMaxValue = 0;
        info.frequencyMinValue = 0;
        info.deviceId = -1;
        info.vibratorId = 1;
        info.position = 0;
        info.isLocal = 1;
        vibratorInfo.push_back(info);
        return HDF_SUCCESS;
    }

    int32_t GetDeviceVibratorInfo(std::vector<HdfVibratorInfoVdi> &vibratorInfo) override
    {
        return GetVibratorInfo(vibratorInfo);
    }

    int32_t GetEffectInfo(const std::string &effectType, HdfEffectInfoVdi &effectInfo) override
    {
        auto it = g_effectDurationMs.find(effectType);
        if (it == g_effectDurationMs.end()) {
            effectInfo.isSupportEffect = false;
            effectInfo.duration = 0;
        } else {
            effectInfo.isSupportEffect = true;
            effectInfo.duration = static_cast<int32_t>(it->second);
        }
        return HDF_SUCCESS;
    }

    int32_t IsVibratorRunning(bool &state) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = running_;
        return HDF_SUCCESS;
    }

    /* DeviceVibratorInfo overloads (V2_0 service paths) — same single motor. */
    int32_t StartOnce(const V2_0::DeviceVibratorInfo &, uint32_t duration) override
    {
        return StartOnce(duration);
    }

    int32_t Start(const V2_0::DeviceVibratorInfo &, const std::string &effectType) override
    {
        return Start(effectType);
    }

    int32_t Stop(const V2_0::DeviceVibratorInfo &, HdfVibratorModeVdi mode) override
    {
        return Stop(mode);
    }

    int32_t GetEffectInfo(const V2_0::DeviceVibratorInfo &, const std::string &effectType,
        HdfEffectInfoVdi &effectInfo) override
    {
        return GetEffectInfo(effectType, effectInfo);
    }

    int32_t IsVibratorRunning(const V2_0::DeviceVibratorInfo &, bool &state) override
    {
        return IsVibratorRunning(state);
    }

private:
    int32_t BuzzLocked(uint32_t durationMs)
    {
        /* duration=0 must still succeed (HATS 0300): treat as no-op pulse. */
        if (durationMs > 0) {
            if (WriteSysfs("duration", std::to_string(durationMs)) != HDF_SUCCESS) {
                return HDF_FAILURE;
            }
        }
        int32_t ret = WriteSysfs("activate", "1");
        if (ret == HDF_SUCCESS) {
            running_ = true;
        }
        return ret;
    }

    std::mutex mutex_;
    bool running_ = false;
};

static int32_t CreateVibratorVdiInstance(struct HdfVdiBase *vdiBase)
{
    if (vdiBase == nullptr) {
        return HDF_FAILURE;
    }
    struct VdiWrapperVibrator *wrapper = reinterpret_cast<struct VdiWrapperVibrator *>(vdiBase);
    wrapper->vibratorModule = new (std::nothrow) HybrisVibratorVdi();
    return wrapper->vibratorModule ? HDF_SUCCESS : HDF_FAILURE;
}

static int32_t DestroyVibratorVdiInstance(struct HdfVdiBase *vdiBase)
{
    if (vdiBase == nullptr) {
        return HDF_FAILURE;
    }
    struct VdiWrapperVibrator *wrapper = reinterpret_cast<struct VdiWrapperVibrator *>(vdiBase);
    auto *impl = static_cast<HybrisVibratorVdi *>(wrapper->vibratorModule);
    delete impl;
    wrapper->vibratorModule = nullptr;
    return HDF_SUCCESS;
}

static struct VdiWrapperVibrator g_hybrisVibratorVdi = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "vibrator_service",
        .CreateVdiInstance = CreateVibratorVdiInstance,
        .DestoryVdiInstance = DestroyVibratorVdiInstance,
    },
    .vibratorModule = nullptr,
};

extern "C" HDF_VDI_INIT(g_hybrisVibratorVdi);

} // namespace V1_1
} // namespace Vibrator
} // namespace HDI
} // namespace OHOS
