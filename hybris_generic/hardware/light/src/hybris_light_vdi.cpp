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
 * Light VDI for hybris_generic: drives indicator LEDs through the plain
 * Linux LED class (/sys/class/leds), replacing the stock VDI that needs the
 * hdf_light kernel driver the Halium kernels don't have. The light list is
 * probed at Init time from the LED names the kernel actually exposes, so one
 * image stays honest on every device: the Volla Phone Plinius has no
 * indicator LED at all (only the panel backlight and the haptics node, which
 * belong to the display and vibrator paths) and reports an empty list, while
 * a device with a standard red/green/blue or white LED gets it published
 * without any per-device configuration.
 */

#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>
#include <hdf_base.h>
#include <hdf_log.h>
#include <securec.h>
#include "v1_0/ilight_interface_vdi.h"

#define HDF_LOG_TAG hybris_light_vdi

namespace OHOS {
namespace HDI {
namespace Light {
namespace V1_0 {
namespace {

constexpr const char *LEDS_DIR = "/sys/class/leds";

/* The light HDI speaks its own status enum (drivers/interface/light
 * LightStatus): NOT_SUPPORT is -1, which HDF_FAILURE happens to equal.
 * HDF_ERR_NOT_SUPPORT (-2) would read as LIGHT_NOT_FLASH to clients. */
constexpr int32_t LIGHT_NOT_SUPPORT = -1;
constexpr int32_t LIGHT_NOT_FLASH = -2;

/* One physical LED channel (one /sys/class/leds entry). */
struct LedChannel {
    std::string dir;        // absolute sysfs directory
    int32_t maxBrightness = 255;
};

/* One published light: a single-color LED or an r/g/b channel triple. */
struct ProbedLight {
    HdfLightInfoVdi info;
    std::vector<LedChannel> channels;   // 1 (single) or 3 (r, g, b)
};

int32_t ReadIntFile(const std::string &path, int32_t defval)
{
    char buf[16] = {0};
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return defval;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return defval;
    }
    int32_t v = atoi(buf);
    return v > 0 ? v : defval;
}

int32_t WriteFile(const std::string &path, const std::string &value)
{
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        HDF_LOGE("%{public}s: open %{public}s failed: %{public}d", __func__, path.c_str(), errno);
        return HDF_FAILURE;
    }
    ssize_t n = write(fd, value.c_str(), value.size());
    close(fd);
    return (n == static_cast<ssize_t>(value.size())) ? HDF_SUCCESS : HDF_FAILURE;
}

} // namespace

class HybrisLightVdi : public ILightInterfaceVdi {
public:
    int32_t Init() override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        lights_.clear();
        Probe();
        HDF_LOGI("%{public}s: probed %{public}zu light(s) under %{public}s",
                 __func__, lights_.size(), LEDS_DIR);
        return HDF_SUCCESS;
    }

    int32_t GetLightInfo(std::vector<HdfLightInfoVdi> &info) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto &entry : lights_) {
            info.push_back(entry.second.info);
        }
        return HDF_SUCCESS;
    }

    int32_t TurnOnLight(int32_t lightId, const HdfLightEffectVdi &effect) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = lights_.find(lightId);
        if (it == lights_.end()) {
            HDF_LOGE("%{public}s: light %{public}d not present on this device", __func__, lightId);
            return LIGHT_NOT_SUPPORT;
        }
        /* Blinking needs the LED-timer trigger; only claim it where the
         * kernel built it (available triggers are listed in the trigger
         * node). Solid on/off always works through brightness. */
        const bool blink = effect.flashEffect.flashMode == VDI_LIGHT_FLASH_BLINK ||
                           effect.flashEffect.flashMode == VDI_LIGHT_FLASH_GRADIENT;
        if (blink && !HasTimerTrigger(it->second)) {
            return LIGHT_NOT_FLASH;
        }
        return Apply(it->second, effect, blink);
    }

    int32_t TurnOnMultiLights(int32_t lightId, const std::vector<HdfLightColorVdi> &colors) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = lights_.find(lightId);
        if (it == lights_.end()) {
            HDF_LOGE("%{public}s: light %{public}d not present on this device", __func__, lightId);
            return LIGHT_NOT_SUPPORT;
        }
        if (colors.empty()) {
            return HDF_ERR_INVALID_PARAM;
        }
        /* The LED class has no per-frame sequencing; apply the first color. */
        HdfLightEffectVdi effect = {};
        effect.lightColor = colors.front();
        return Apply(it->second, effect, false);
    }

    int32_t TurnOffLight(int32_t lightId) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = lights_.find(lightId);
        if (it == lights_.end()) {
            return LIGHT_NOT_SUPPORT;
        }
        int32_t ret = HDF_SUCCESS;
        for (const auto &ch : it->second.channels) {
            (void)WriteFile(ch.dir + "/trigger", "none");
            if (WriteFile(ch.dir + "/brightness", "0") != HDF_SUCCESS) {
                ret = HDF_FAILURE;
            }
        }
        return ret;
    }

private:
    /* Only names that unambiguously identify an indicator LED are published.
     * Everything else in /sys/class/leds stays with its real owner: the
     * panel backlight (display), the haptics node (vibrator VDI), camera
     * flash, storage activity, keyboard backlights. */
    void Probe()
    {
        DIR *d = opendir(LEDS_DIR);
        if (d == nullptr) {
            return;
        }
        std::set<std::string> names;
        struct dirent *de;
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] != '.') {
                names.emplace(de->d_name);
            }
        }
        closedir(d);

        auto channel = [](const std::string &name) {
            LedChannel ch;
            ch.dir = std::string(LEDS_DIR) + "/" + name;
            ch.maxBrightness = ReadIntFile(ch.dir + "/max_brightness", 255);
            return ch;
        };

        if (names.count("red") != 0 && names.count("green") != 0 && names.count("blue") != 0) {
            ProbedLight l;
            l.info.lightId = VDI_LIGHT_ID_NOTIFICATIONS;
            l.info.lightType = VDI_LIGHT_TYPE_RGB_COLOR;
            l.info.lightNumber = 1;
            (void)strcpy_s(l.info.lightName, LIGHT_NAME_MAX_LEN, "rgb-indicator");
            l.channels = { channel("red"), channel("green"), channel("blue") };
            lights_[l.info.lightId] = l;
        } else if (names.count("white") != 0) {
            ProbedLight l;
            l.info.lightId = VDI_LIGHT_ID_NOTIFICATIONS;
            l.info.lightType = VDI_LIGHT_TYPE_SINGLE_COLOR;
            l.info.lightNumber = 1;
            (void)strcpy_s(l.info.lightName, LIGHT_NAME_MAX_LEN, "white-indicator");
            l.channels = { channel("white") };
            lights_[l.info.lightId] = l;
        }
    }

    static bool HasTimerTrigger(const ProbedLight &light)
    {
        if (light.channels.empty()) {
            return false;
        }
        char buf[512] = {0};
        int fd = open((light.channels[0].dir + "/trigger").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        return n > 0 && strstr(buf, "timer") != nullptr;
    }

    int32_t Apply(const ProbedLight &light, const HdfLightEffectVdi &effect, bool blink)
    {
        /* Channel intensities: single-color LEDs use the R byte as overall
         * brightness (0 means full on, matching the framework's "default
         * color" convention for single-color lights). */
        uint8_t rgb[3] = {
            effect.lightColor.colorValue.rgbColor.r,
            effect.lightColor.colorValue.rgbColor.g,
            effect.lightColor.colorValue.rgbColor.b,
        };
        int32_t ret = HDF_SUCCESS;
        for (size_t i = 0; i < light.channels.size(); i++) {
            const LedChannel &ch = light.channels[i];
            uint32_t byte = (light.channels.size() == 1)
                ? (rgb[0] != 0 || rgb[1] != 0 || rgb[2] != 0 ? rgb[0] : UINT8_MAX)
                : rgb[i];
            uint32_t level = byte * static_cast<uint32_t>(ch.maxBrightness) / UINT8_MAX;
            if (blink) {
                (void)WriteFile(ch.dir + "/trigger", "timer");
                (void)WriteFile(ch.dir + "/delay_on", std::to_string(effect.flashEffect.onTime));
                (void)WriteFile(ch.dir + "/delay_off", std::to_string(effect.flashEffect.offTime));
            } else {
                (void)WriteFile(ch.dir + "/trigger", "none");
            }
            if (WriteFile(ch.dir + "/brightness", std::to_string(level)) != HDF_SUCCESS) {
                ret = HDF_FAILURE;
            }
        }
        return ret;
    }

    std::mutex mutex_;
    std::map<int32_t, ProbedLight> lights_;
};

static int32_t CreateLightVdiInstance(struct HdfVdiBase *vdiBase)
{
    if (vdiBase == nullptr) {
        return HDF_FAILURE;
    }
    struct VdiWrapperLight *wrapper = reinterpret_cast<struct VdiWrapperLight *>(vdiBase);
    wrapper->lightModule = new (std::nothrow) HybrisLightVdi();
    return wrapper->lightModule != nullptr ? HDF_SUCCESS : HDF_FAILURE;
}

static int32_t DestroyLightVdiInstance(struct HdfVdiBase *vdiBase)
{
    if (vdiBase == nullptr) {
        return HDF_FAILURE;
    }
    struct VdiWrapperLight *wrapper = reinterpret_cast<struct VdiWrapperLight *>(vdiBase);
    auto *impl = static_cast<HybrisLightVdi *>(wrapper->lightModule);
    delete impl;
    wrapper->lightModule = nullptr;
    return HDF_SUCCESS;
}

static struct VdiWrapperLight g_hybrisLightVdi = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "light_service",
        .CreateVdiInstance = CreateLightVdiInstance,
        .DestoryVdiInstance = DestroyLightVdiInstance,
    },
    .lightModule = nullptr,
};

extern "C" HDF_VDI_INIT(g_hybrisLightVdi);

} // namespace V1_0
} // namespace Light
} // namespace HDI
} // namespace OHOS
