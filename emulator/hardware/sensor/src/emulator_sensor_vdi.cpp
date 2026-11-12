/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sensor VDI for the emulator: a fixed set of virtual sensors whose readings
 * come from the system parameters emulator.sensor.<name> (defaults in
 * emulator_sensor.para), so a host can simulate them over hdc:
 *
 *   hdc shell param set emulator.sensor.accelerometer "0 9.81 0"
 *
 * A value is whitespace- or comma-separated floats in the units and order of
 * the JS sensor API; missing values read as 0. Continuous sensors report at
 * the requested sampling interval; on-change sensors (light, proximity,
 * humidity, temperature) report when enabled and whenever their value
 * changes, and the sensor service replays the last value to late subscribers.
 *
 * Without this, sensor_host publishes no service on x86 (the stock VDI needs
 * kernel HDF sensor drivers), and the sensor service spends 25 x 200 ms
 * retrying it at boot, which holds up the power service and DPMS.
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <hdf_base.h>
#include <hdf_log.h>
#include "isensor_interface_vdi.h"
#include "param/sys_param.h"

#define HDF_LOG_TAG emulator_sensor_vdi

namespace OHOS {
namespace HDI {
namespace Sensor {
namespace V1_1 {
namespace {

constexpr int64_t NS_PER_MS = 1000000;
constexpr int64_t DEFAULT_SAMPLING_NS = 200 * NS_PER_MS;
constexpr int64_t MIN_SAMPLING_NS = 10 * NS_PER_MS;
constexpr int64_t MAX_SAMPLING_NS = 1000 * NS_PER_MS;
constexpr int64_t ON_CHANGE_POLL_NS = 100 * NS_PER_MS;
constexpr int32_t GROUP_TRADITIONAL = 0;
constexpr int32_t GROUP_MEDICAL = 1;
constexpr int32_t MAX_VALUES = 3;
constexpr int32_t MODE_REALTIME = 1;   /* SensorModeType SENSOR_MODE_REALTIME */
constexpr int32_t MODE_ON_CHANGE = 2;  /* SensorModeType SENSOR_MODE_ON_CHANGE */

struct VirtualSensor {
    int32_t type;
    const char *name;  /* emulator.sensor.<name> */
    int valueCount;    /* floats per sample, as sensor_napi_utils expects */
    bool onChange;
    float maxRange;
    float resolution;
    float power;
};
/* No hall sensor: the power service would treat it as a lid. */
constexpr VirtualSensor SENSORS[] = {
    { HDF_SENSOR_TYPE_ACCELEROMETER, "accelerometer", 3, false, 78.4f, 0.01f, 0.2f },
    { HDF_SENSOR_TYPE_GYROSCOPE, "gyroscope", 3, false, 34.9f, 0.001f, 0.5f },
    { HDF_SENSOR_TYPE_MAGNETIC_FIELD, "magnetic_field", 3, false, 2000.0f, 0.1f, 0.5f },
    { HDF_SENSOR_TYPE_AMBIENT_LIGHT, "ambient_light", 3, true, 65535.0f, 1.0f, 0.1f },
    { HDF_SENSOR_TYPE_BAROMETER, "barometer", 1, false, 1100.0f, 0.01f, 0.1f },
    { HDF_SENSOR_TYPE_PROXIMITY, "proximity", 1, true, 5.0f, 5.0f, 0.1f },
    { HDF_SENSOR_TYPE_HUMIDITY, "humidity", 1, true, 100.0f, 0.1f, 0.1f },
    { HDF_SENSOR_TYPE_AMBIENT_TEMPERATURE, "ambient_temperature", 1, true, 85.0f, 0.1f, 0.1f },
};

const VirtualSensor *FindSensor(int32_t type)
{
    for (const auto &s : SENSORS) {
        if (s.type == type) {
            return &s;
        }
    }
    return nullptr;
}

int64_t BoottimeNs()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void ParseValues(const char *text, float values[MAX_VALUES])
{
    std::fill(values, values + MAX_VALUES, 0.0f);
    const char *p = text;
    for (int i = 0; p != nullptr && i < MAX_VALUES; i++) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        char *end = nullptr;
        float v = strtof(p, &end);
        if (end == p) {
            break;
        }
        values[i] = v;
        p = end;
    }
}

} // namespace

class EmulatorSensorVdi : public ISensorInterfaceVdi {
public:
    ~EmulatorSensorVdi() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    int32_t Init() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return HDF_SUCCESS;
        }
        for (const auto &s : SENSORS) {
            HdfSensorInformationVdi info = {};
            info.sensorName = std::string("Emulator ") + s.name;
            info.vendorName = "Oniro";
            info.firmwareVersion = "1.0";
            info.hardwareVersion = "1.0";
            info.sensorTypeId = s.type;
            info.sensorId = s.type;
            info.sensorHandle = { -1, s.type, 0, 1 };
            info.maxRange = s.maxRange;
            info.accuracy = s.resolution;
            info.power = s.power;
            info.minDelay = MIN_SAMPLING_NS;
            info.maxDelay = MAX_SAMPLING_NS;
            info.fifoMaxEventCount = 0;
            infoList_.push_back(info);
            states_[s.type].sensor = &s;
        }
        running_ = true;
        worker_ = std::thread([this] { WorkerLoop(); });
        HDF_LOGI("%{public}s: %{public}zu virtual sensors", __func__, infoList_.size());
        return HDF_SUCCESS;
    }

    int32_t GetAllSensorInfo(std::vector<HdfSensorInformationVdi> &info) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
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
        if (FindSensor(sensorId) == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            SensorState &st = states_[sensorId];
            st.enabled = true;
            st.reported = false;
            st.nextNs = 0;
        }
        cv_.notify_all();
        HDF_LOGI("%{public}s: %{public}d", __func__, sensorId);
        return HDF_SUCCESS;
    }

    int32_t Disable(int32_t sensorId) override
    {
        if (FindSensor(sensorId) == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        states_[sensorId].enabled = false;
        HDF_LOGI("%{public}s: %{public}d", __func__, sensorId);
        return HDF_SUCCESS;
    }

    int32_t SetBatch(int32_t sensorId, int64_t samplingInterval, int64_t reportInterval) override
    {
        (void)reportInterval;
        if (FindSensor(sensorId) == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        if (samplingInterval < 0 || reportInterval < 0) {
            return HDF_ERR_INVALID_PARAM;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            SensorState &st = states_[sensorId];
            st.intervalNs = samplingInterval > 0 ?
                std::clamp(samplingInterval, MIN_SAMPLING_NS, MAX_SAMPLING_NS) : DEFAULT_SAMPLING_NS;
            st.nextNs = 0;
        }
        cv_.notify_all();
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
        if (FindSensor(sensorId) == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        if (mode != MODE_REALTIME && mode != MODE_ON_CHANGE) {
            return HDF_FAILURE;
        }
        return HDF_SUCCESS;
    }

    int32_t SetOption(int32_t sensorId, uint32_t option) override
    {
        (void)option;
        if (FindSensor(sensorId) == nullptr) {
            return HDF_ERR_NOT_SUPPORT;
        }
        return HDF_SUCCESS;
    }

    int32_t Register(int32_t groupId, const sptr<ISensorCallbackVdi> &callbackObj) override
    {
        if (groupId < GROUP_TRADITIONAL || groupId > GROUP_MEDICAL || callbackObj == nullptr) {
            return HDF_ERR_INVALID_PARAM;
        }
        std::lock_guard<std::mutex> lock(cbMutex_);
        auto &list = (groupId == GROUP_TRADITIONAL) ? callbacks_ : medicalCallbacks_;
        if (std::find(list.begin(), list.end(), callbackObj) == list.end()) {
            list.push_back(callbackObj);
        }
        return HDF_SUCCESS;
    }

    int32_t Unregister(int32_t groupId, const sptr<ISensorCallbackVdi> &callbackObj) override
    {
        if (groupId < GROUP_TRADITIONAL || groupId > GROUP_MEDICAL) {
            return HDF_ERR_INVALID_PARAM;
        }
        std::lock_guard<std::mutex> lock(cbMutex_);
        auto &list = (groupId == GROUP_TRADITIONAL) ? callbacks_ : medicalCallbacks_;
        list.erase(std::remove(list.begin(), list.end(), callbackObj), list.end());
        return HDF_SUCCESS;
    }

private:
    struct SensorState {
        const VirtualSensor *sensor = nullptr;
        bool enabled = false;
        bool reported = false;  /* on-change: first sample since Enable sent */
        int64_t intervalNs = DEFAULT_SAMPLING_NS;
        int64_t nextNs = 0;
    };

    struct Sample {
        const VirtualSensor *sensor;
        float values[MAX_VALUES];
    };

    /* Only this thread touches the cached parameter handles. */
    void WorkerLoop()
    {
        std::map<int32_t, CachedHandle> handles;
        for (const auto &s : SENSORS) {
            std::string key = std::string("emulator.sensor.") + s.name;
            handles[s.type] = CachedParameterCreate(key.c_str(), "");
            if (handles[s.type] == nullptr) {
                HDF_LOGW("%{public}s: cannot read %{public}s, the sensor reports zeros", __func__, key.c_str());
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        while (running_) {
            int64_t now = BoottimeNs();
            int64_t wakeNs = INT64_MAX;
            std::vector<Sample> due;
            for (auto &[type, st] : states_) {
                const char *text = nullptr;
                int changed = 0;
                if (handles[type] != nullptr) {
                    text = CachedParameterGetChanged(handles[type], &changed);
                }
                if (!st.enabled) {
                    continue;
                }
                bool send = st.sensor->onChange ? (!st.reported || changed != 0) : (now >= st.nextNs);
                if (send) {
                    Sample sample = { st.sensor, {} };
                    ParseValues(text != nullptr ? text : "", sample.values);
                    due.push_back(sample);
                    st.reported = true;
                    st.nextNs = now + st.intervalNs;
                }
                wakeNs = std::min(wakeNs, st.sensor->onChange ? now + ON_CHANGE_POLL_NS : st.nextNs);
            }
            if (!due.empty()) {
                lock.unlock();
                for (const auto &sample : due) {
                    Report(sample, now);
                }
                lock.lock();
                continue;
            }
            if (wakeNs == INT64_MAX) {
                cv_.wait(lock); /* nothing enabled: Enable() wakes us */
            } else {
                cv_.wait_for(lock, std::chrono::nanoseconds(std::max<int64_t>(wakeNs - now, NS_PER_MS)));
            }
        }
        lock.unlock();
        for (auto &[type, handle] : handles) {
            (void)type;
            if (handle != nullptr) {
                CachedParameterDestroy(handle);
            }
        }
    }

    void Report(const Sample &sample, int64_t timestamp)
    {
        HdfSensorEventsVdi event = {};
        event.sensorId = sample.sensor->type;
        event.deviceSensorInfo = { -1, sample.sensor->type, 0, 1 };
        event.version = 1;
        event.timestamp = timestamp;
        event.option = 0;
        event.mode = sample.sensor->onChange ? MODE_ON_CHANGE : MODE_REALTIME;
        event.dataLen = static_cast<uint32_t>(sample.sensor->valueCount * sizeof(float));
        event.data.resize(event.dataLen);
        std::copy_n(reinterpret_cast<const uint8_t *>(sample.values), event.dataLen, event.data.begin());

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
    std::condition_variable cv_;
    bool running_ = false;
    std::thread worker_;
    std::vector<HdfSensorInformationVdi> infoList_;
    std::map<int32_t, SensorState> states_;
    std::mutex cbMutex_;
    std::vector<sptr<ISensorCallbackVdi>> callbacks_;
    std::vector<sptr<ISensorCallbackVdi>> medicalCallbacks_;
};

static int32_t CreateSensorVdiInstance(struct HdfVdiBase *vdiBase)
{
    if (vdiBase == nullptr) {
        return HDF_FAILURE;
    }
    struct WrapperSensorVdi *wrapper = reinterpret_cast<struct WrapperSensorVdi *>(vdiBase);
    wrapper->sensorModule = new (std::nothrow) EmulatorSensorVdi();
    return wrapper->sensorModule ? HDF_SUCCESS : HDF_FAILURE;
}

static int32_t DestroySensorVdiInstance(struct HdfVdiBase *vdiBase)
{
    if (vdiBase == nullptr) {
        return HDF_FAILURE;
    }
    struct WrapperSensorVdi *wrapper = reinterpret_cast<struct WrapperSensorVdi *>(vdiBase);
    delete static_cast<EmulatorSensorVdi *>(wrapper->sensorModule);
    wrapper->sensorModule = nullptr;
    return HDF_SUCCESS;
}

static struct WrapperSensorVdi g_emulatorSensorVdi = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "sensor_Service",
        .CreateVdiInstance = CreateSensorVdiInstance,
        .DestoryVdiInstance = DestroySensorVdiInstance,
    },
    .sensorModule = nullptr,
};

extern "C" HDF_VDI_INIT(g_emulatorSensorVdi);

} // namespace V1_1
} // namespace Sensor
} // namespace HDI
} // namespace OHOS
