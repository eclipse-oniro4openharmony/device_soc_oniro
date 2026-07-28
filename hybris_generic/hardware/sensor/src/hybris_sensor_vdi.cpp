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
 * Sensor VDI for hybris_generic backed by the MTK sensorhub 2.0 character
 * device /dev/hf_manager (the Halium kernel's mediatek sensor stack). Each
 * open() is an independent hf_client with its own event FIFO: commands are
 * written as struct hf_manager_cmd, samples read back as hf_manager_event,
 * and sensors enumerated with the SENSOR_INFO ioctl. Raw values are integers
 * scaled by the per-sensor `gain` reported in sensor_info.
 */

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <hdf_base.h>
#include <hdf_log.h>
#include "isensor_interface_vdi.h"

#define HDF_LOG_TAG hybris_sensor_vdi

namespace OHOS {
namespace HDI {
namespace Sensor {
namespace V1_1 {
namespace {

constexpr const char *HF_DEV = "/dev/hf_manager";

/* MTK sensor/2.0 userspace ABI (drivers/misc/mediatek/sensor/2.0/core),
 * verified against the ansuz kernel module by on-device probe. */
#pragma pack(push, 4)
struct HfManagerCmd {
    uint8_t sensorType;
    uint8_t actionDownSample; /* action:4 | down_sample:1 */
    uint8_t length;
    uint8_t padding[1];
    int8_t data[48];
};
struct HfManagerEvent {
    int64_t timestamp;
    uint8_t sensorType;
    uint8_t accurancy;
    uint8_t action;
    uint8_t padding[1];
    int32_t word[16];
};
struct HfSensorInfo {
    uint8_t sensorType;
    uint8_t padding[3];
    uint32_t gain;
    char name[16];
    char vendor[16];
};
struct HfIoctlPacket {
    uint8_t sensorType;
    uint8_t padding[3];
    union {
        uint8_t status;
        int8_t byte[64];
    };
};
struct HfManagerBatch {
    int64_t delay;
    int64_t latency;
};
#pragma pack(pop)

#define HF_MANAGER_REQUEST_SENSOR_INFO _IOWR('a', 6, struct HfIoctlPacket)

constexpr uint8_t HF_ACTION_DISABLE = 0;
constexpr uint8_t HF_ACTION_ENABLE = 1;
constexpr uint8_t HF_DATA_ACTION = 0;

constexpr int64_t DEFAULT_SAMPLING_NS = 20000000; /* 50 Hz */
constexpr int32_t GROUP_TRADITIONAL = 0;
constexpr int32_t GROUP_MEDICAL = 1;

/* MTK hf_manager sensor type → OHOS HdfSensorTypeTag, with the number of
 * meaningful words per sample. Only real physical sensors are exposed; the
 * hub's fusion outputs stay hidden (the framework runs its own fusion). */
struct SensorMapEntry {
    uint8_t mtkType;
    int32_t ohosType;
    int valueCount;   /* words carried by the hub event */
    int reportCount;  /* floats the framework expects (sensor_napi_utils
                       * g_sensorAttributeList; light needs 3: intensity,
                       * colorTemperature, infraredLuminance) — extra
                       * values are zero-padded */
    float maxRange;
    float power;
};
constexpr SensorMapEntry SENSOR_MAP[] = {
    { 1, HDF_SENSOR_TYPE_ACCELEROMETER, 3, 3, 39.2f, 0.5f },
    { 4, HDF_SENSOR_TYPE_GYROSCOPE, 3, 3, 34.9f, 0.9f },
    { 2, HDF_SENSOR_TYPE_MAGNETIC_FIELD, 3, 3, 4900.0f, 0.5f },
    { 5, HDF_SENSOR_TYPE_AMBIENT_LIGHT, 1, 3, 65535.0f, 0.1f },
    { 8, HDF_SENSOR_TYPE_PROXIMITY, 1, 1, 5.0f, 0.1f },
};

const SensorMapEntry *FindByOhosType(int32_t ohosType)
{
    for (const auto &e : SENSOR_MAP) {
        if (e.ohosType == ohosType) {
            return &e;
        }
    }
    return nullptr;
}

const SensorMapEntry *FindByMtkType(uint8_t mtkType)
{
    for (const auto &e : SENSOR_MAP) {
        if (e.mtkType == mtkType) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

class HybrisSensorVdi : public ISensorInterfaceVdi {
public:
    ~HybrisSensorVdi() override
    {
        running_ = false;
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (reader_.joinable()) {
            reader_.detach(); /* blocked in read(); service lifetime == process lifetime */
        }
    }

    int32_t Init() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        /* The devhost binds this service early in boot, and /dev/hf_manager
         * may not exist or be chowned yet. Never fail Init for that — the
         * service would stay unregistered until a host restart. Open lazily
         * and let every entry point retry. */
        if (EnsureOpenLocked() != HDF_SUCCESS) {
            HDF_LOGW("%{public}s: %{public}s not ready yet, will retry lazily", __func__, HF_DEV);
        }
        return HDF_SUCCESS;
    }

    int32_t GetAllSensorInfo(std::vector<HdfSensorInformationVdi> &info) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)EnsureOpenLocked();
        info = infoList_;
        return HDF_SUCCESS;
    }

    int32_t GetDeviceSensorInfo(int32_t deviceId, std::vector<HdfSensorInformationVdi> &info) override
    {
        (void)deviceId;
        return GetAllSensorInfo(info);
    }

    int32_t Enable(int32_t sensorId) override
    {
        const SensorMapEntry *e = FindByOhosType(sensorId);
        if (e == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (EnsureOpenLocked() != HDF_SUCCESS) {
            return HDF_FAILURE;
        }
        int64_t sampling = DEFAULT_SAMPLING_NS;
        auto it = batch_.find(sensorId);
        if (it != batch_.end() && it->second.samplingInterval > 0) {
            sampling = it->second.samplingInterval;
        }
        int32_t ret = SendCmdLocked(e->mtkType, HF_ACTION_ENABLE, sampling, 0);
        if (ret == HDF_SUCCESS) {
            enabled_[sensorId] = true;
        }
        return ret;
    }

    int32_t Disable(int32_t sensorId) override
    {
        const SensorMapEntry *e = FindByOhosType(sensorId);
        if (e == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_[sensorId] = false;
        if (fd_ < 0) {
            return HDF_SUCCESS; /* nothing was ever enabled */
        }
        return SendCmdLocked(e->mtkType, HF_ACTION_DISABLE, 0, 0);
    }

    int32_t SetBatch(int32_t sensorId, int64_t samplingInterval, int64_t reportInterval) override
    {
        const SensorMapEntry *e = FindByOhosType(sensorId);
        if (e == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        if (samplingInterval < 0 || reportInterval < 0) {
            return HDF_ERR_INVALID_PARAM;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        batch_[sensorId] = { samplingInterval, reportInterval };
        if (enabled_[sensorId]) {
            /* rate change on a live sensor: re-issue the enable */
            return SendCmdLocked(e->mtkType, HF_ACTION_ENABLE,
                samplingInterval > 0 ? samplingInterval : DEFAULT_SAMPLING_NS, reportInterval);
        }
        return HDF_SUCCESS;
    }

    int32_t SetSaBatch(int32_t sensorId, int64_t samplingInterval, int64_t reportInterval) override
    {
        (void)sensorId;
        (void)samplingInterval;
        (void)reportInterval;
        return HDF_SUCCESS;
    }

    int32_t SetMode(int32_t sensorId, int32_t mode) override
    {
        if (FindByOhosType(sensorId) == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        /* Only realtime reporting exists on this hub path. */
        constexpr int32_t SENSOR_MODE_REALTIME = 1;
        constexpr int32_t SENSOR_MODE_ON_CHANGE = 2;
        if (mode != SENSOR_MODE_REALTIME && mode != SENSOR_MODE_ON_CHANGE) {
            return HDF_FAILURE;
        }
        return HDF_SUCCESS;
    }

    int32_t SetOption(int32_t sensorId, uint32_t option) override
    {
        (void)option;
        if (FindByOhosType(sensorId) == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        return HDF_SUCCESS;
    }

    int32_t Register(int32_t groupId, const sptr<ISensorCallbackVdi> &callbackObj) override
    {
        if (groupId < GROUP_TRADITIONAL || groupId > GROUP_MEDICAL) {
            return HDF_ERR_INVALID_PARAM;
        }
        if (callbackObj == nullptr) {
            return HDF_ERR_INVALID_PARAM;
        }
        std::lock_guard<std::mutex> lock(cbMutex_);
        auto &list = (groupId == GROUP_TRADITIONAL) ? callbacks_ : medicalCallbacks_;
        for (const auto &cb : list) {
            if (cb == callbackObj) {
                return HDF_SUCCESS;
            }
        }
        list.push_back(callbackObj);
        return HDF_SUCCESS;
    }

    int32_t Unregister(int32_t groupId, const sptr<ISensorCallbackVdi> &callbackObj) override
    {
        if (groupId < GROUP_TRADITIONAL || groupId > GROUP_MEDICAL) {
            return HDF_ERR_INVALID_PARAM;
        }
        std::lock_guard<std::mutex> lock(cbMutex_);
        auto &list = (groupId == GROUP_TRADITIONAL) ? callbacks_ : medicalCallbacks_;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (*it == callbackObj) {
                list.erase(it);
                return HDF_SUCCESS;
            }
        }
        return HDF_SUCCESS;
    }

private:
    int32_t EnsureOpenLocked()
    {
        if (fd_ >= 0) {
            return HDF_SUCCESS;
        }
        fd_ = open(HF_DEV, O_RDWR);
        if (fd_ < 0) {
            return HDF_FAILURE;
        }
        EnumerateLocked();
        if (infoList_.empty()) {
            HDF_LOGE("%{public}s: no usable sensors on %{public}s", __func__, HF_DEV);
            close(fd_);
            fd_ = -1;
            return HDF_FAILURE;
        }
        running_ = true;
        reader_ = std::thread([this] { ReaderLoop(); });
        HDF_LOGI("%{public}s: %{public}zu sensors ready", __func__, infoList_.size());
        return HDF_SUCCESS;
    }

    void EnumerateLocked()
    {
        for (const auto &e : SENSOR_MAP) {
            HfIoctlPacket pkt = {};
            pkt.sensorType = e.mtkType;
            if (ioctl(fd_, HF_MANAGER_REQUEST_SENSOR_INFO, &pkt) != 0) {
                continue;
            }
            HfSensorInfo raw = {};
            memcpy(&raw, pkt.byte, sizeof(raw));
            uint32_t gain = raw.gain > 0 ? raw.gain : 1;
            gain_[e.mtkType] = gain;

            HdfSensorInformationVdi info = {};
            info.sensorName = std::string(raw.name, strnlen(raw.name, sizeof(raw.name)));
            info.vendorName = std::string(raw.vendor, strnlen(raw.vendor, sizeof(raw.vendor)));
            info.firmwareVersion = "1.0";
            info.hardwareVersion = "1.0";
            info.sensorTypeId = e.ohosType;
            info.sensorId = e.ohosType;
            info.sensorHandle = { -1, e.ohosType, 0, 1 };
            info.maxRange = e.maxRange;
            info.accuracy = 1.0f / static_cast<float>(gain);
            info.power = e.power;
            info.minDelay = 5000000;    /* 5 ms */
            info.maxDelay = 1000000000; /* 1 s */
            info.fifoMaxEventCount = 0;
            infoList_.push_back(info);
            HDF_LOGI("%{public}s: mtk=%{public}u ohos=%{public}d name=%{public}s gain=%{public}u",
                __func__, e.mtkType, e.ohosType, info.sensorName.c_str(), gain);
        }
    }

    int32_t SendCmdLocked(uint8_t mtkType, uint8_t action, int64_t delayNs, int64_t latencyNs)
    {
        if (fd_ < 0) {
            return HDF_FAILURE;
        }
        HfManagerCmd cmd = {};
        cmd.sensorType = mtkType;
        cmd.actionDownSample = action & 0x0F;
        if (action == HF_ACTION_ENABLE) {
            HfManagerBatch batch = { delayNs, latencyNs };
            memcpy(cmd.data, &batch, sizeof(batch));
            cmd.length = sizeof(batch);
        }
        ssize_t n = write(fd_, &cmd, sizeof(cmd));
        if (n < 0) {
            HDF_LOGE("%{public}s: cmd type=%{public}u action=%{public}u failed: %{public}d",
                __func__, mtkType, action, errno);
            return HDF_FAILURE;
        }
        return HDF_SUCCESS;
    }

    void ReaderLoop()
    {
        HfManagerEvent events[8];
        int errStreak = 0;
        while (running_) {
            /* hf_manager returns 0 from read() while this client has no
             * enabled sensors — that is NOT EOF. Wait for readability and
             * treat empty reads as idle, or the reader dies at boot before
             * the first Enable and every later sample is dropped on the
             * kernel side ("buffer reset" kmsg spam). */
            struct pollfd pfd = { fd_, POLLIN, 0 };
            int pr = poll(&pfd, 1, 200);
            if (pr <= 0) {
                continue;
            }
            ssize_t n = read(fd_, events, sizeof(events));
            if (n <= 0) {
                if (n < 0 && errno != EINTR && errno != EAGAIN) {
                    if (++errStreak == 1 || errStreak % 500 == 0) {
                        HDF_LOGE("%{public}s: read failed: %{public}d (streak %{public}d)",
                            __func__, errno, errStreak);
                    }
                    usleep(20000);
                }
                continue;
            }
            errStreak = 0;
            int count = static_cast<int>(n / static_cast<ssize_t>(sizeof(events[0])));
            for (int i = 0; i < count; i++) {
                DispatchEvent(events[i]);
            }
        }
        HDF_LOGI("%{public}s: reader exiting", __func__);
    }

    void DispatchEvent(const HfManagerEvent &ev)
    {
        if (ev.action != HF_DATA_ACTION) {
            return;
        }
        const SensorMapEntry *e = FindByMtkType(ev.sensorType);
        if (e == nullptr) {
            return;
        }
        uint32_t gain = 1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = gain_.find(ev.sensorType);
            if (it != gain_.end()) {
                gain = it->second;
            }
        }

        float values[3] = {};
        for (int i = 0; i < e->valueCount; i++) {
            values[i] = static_cast<float>(ev.word[i]) / static_cast<float>(gain);
        }

        HdfSensorEventsVdi event = {};
        event.sensorId = e->ohosType;
        event.deviceSensorInfo = { -1, e->ohosType, 0, 1 };
        event.version = 1;
        event.timestamp = ev.timestamp;
        event.option = 0;
        event.mode = 1; /* realtime */
        event.dataLen = static_cast<uint32_t>(e->reportCount * sizeof(float));
        event.data.resize(event.dataLen);
        memcpy(event.data.data(), values, event.dataLen);

        std::vector<sptr<ISensorCallbackVdi>> cbs;
        {
            std::lock_guard<std::mutex> lock(cbMutex_);
            cbs = callbacks_;
        }
        for (const auto &cb : cbs) {
            if (cb != nullptr) {
                cb->OnDataEventVdi(event);
            }
        }
    }

    std::mutex mutex_;
    std::mutex cbMutex_;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread reader_;
    std::vector<HdfSensorInformationVdi> infoList_;
    std::map<uint8_t, uint32_t> gain_;
    std::map<int32_t, SensorInterval> batch_;
    std::map<int32_t, bool> enabled_;
    std::vector<sptr<ISensorCallbackVdi>> callbacks_;
    std::vector<sptr<ISensorCallbackVdi>> medicalCallbacks_;
};

static int32_t CreateSensorVdiInstance(struct HdfVdiBase *vdiBase)
{
    if (vdiBase == nullptr) {
        return HDF_FAILURE;
    }
    struct WrapperSensorVdi *wrapper = reinterpret_cast<struct WrapperSensorVdi *>(vdiBase);
    wrapper->sensorModule = new (std::nothrow) HybrisSensorVdi();
    return wrapper->sensorModule ? HDF_SUCCESS : HDF_FAILURE;
}

static int32_t DestroySensorVdiInstance(struct HdfVdiBase *vdiBase)
{
    if (vdiBase == nullptr) {
        return HDF_FAILURE;
    }
    struct WrapperSensorVdi *wrapper = reinterpret_cast<struct WrapperSensorVdi *>(vdiBase);
    auto *impl = static_cast<HybrisSensorVdi *>(wrapper->sensorModule);
    delete impl;
    wrapper->sensorModule = nullptr;
    return HDF_SUCCESS;
}

static struct WrapperSensorVdi g_hybrisSensorVdi = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "sensor_Service",
        .CreateVdiInstance = CreateSensorVdiInstance,
        .DestoryVdiInstance = DestroySensorVdiInstance,
    },
    .sensorModule = nullptr,
};

extern "C" HDF_VDI_INIT(g_hybrisSensorVdi);

} // namespace V1_1
} // namespace Sensor
} // namespace HDI
} // namespace OHOS
