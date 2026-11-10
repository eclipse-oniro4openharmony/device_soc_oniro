/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * vendorGnssAdapter.so for the emulator: a simulated GNSS receiver whose
 * position comes from the system parameter emulator.gnss.location (default
 * in emulator_gnss.para), so a host can move the device over hdc:
 *
 *   hdc shell param set emulator.gnss.location "48.8584 2.2945 35"
 *
 * The value is "latitude longitude [altitude [speed [bearing [accuracy]]]]"
 * in degrees, metres, m/s, degrees and metres, separated by spaces or
 * commas. An empty value means "no fix": the receiver still navigates and
 * reports satellites, but no position.
 *
 * location_host's GNSS, AGNSS and geofence HDIs each dlopen this library and
 * dlsym `GnssVendorInterface` (drivers/peripheral/location/gnss/hdi_service/
 * location_vendor_interface.cpp). While a session runs, a worker thread sends
 * a fix, NMEA (GGA, RMC) and a satellite list at the requested interval.
 */

#include <deque>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <hilog/log.h>
#include "location_vendor_lib.h"
#include "parameter.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "EmulatorGnss"

#define EG_LOGI(fmt, ...) HILOG_INFO(LOG_CORE, fmt, ##__VA_ARGS__)
#define EG_LOGW(fmt, ...) HILOG_WARN(LOG_CORE, fmt, ##__VA_ARGS__)

namespace OHOS {
namespace HDI {
namespace Location {
namespace {

constexpr const char *LOCATION_PARAM = "emulator.gnss.location";
constexpr uint32_t DEFAULT_INTERVAL_MS = 1000;
constexpr uint32_t MIN_INTERVAL_MS = 100;
constexpr float DEFAULT_ACCURACY_M = 5.0f;
constexpr int VENDOR_OK = 0;
constexpr int VENDOR_FAIL = -1;
constexpr int PARAM_MAX = 256;
constexpr int NMEA_MAX = 128;

struct Fix {
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    float speed = 0.0f;
    float bearing = 0.0f;
    float accuracy = DEFAULT_ACCURACY_M;
    bool hasAltitude = false;
    bool hasSpeed = false;
    bool hasBearing = false;
};

/* A fixed sky: GPS and Galileo satellites at plausible positions. */
struct Satellite {
    int16_t id;
    ConstellationCategory constellation;
    float elevation;
    float azimuth;
    float cn0;
    float carrierHz;
};

constexpr float GPS_L1_HZ = 1575.42e6f;
constexpr Satellite SKY[] = {
    { 2, ConstellationCategory::CONSTELLATION_CATEGORY_GPS, 62.0f, 45.0f, 38.0f, GPS_L1_HZ },
    { 5, ConstellationCategory::CONSTELLATION_CATEGORY_GPS, 35.0f, 120.0f, 34.0f, GPS_L1_HZ },
    { 12, ConstellationCategory::CONSTELLATION_CATEGORY_GPS, 48.0f, 210.0f, 36.0f, GPS_L1_HZ },
    { 15, ConstellationCategory::CONSTELLATION_CATEGORY_GPS, 22.0f, 300.0f, 29.0f, GPS_L1_HZ },
    { 25, ConstellationCategory::CONSTELLATION_CATEGORY_GPS, 75.0f, 330.0f, 41.0f, GPS_L1_HZ },
    { 29, ConstellationCategory::CONSTELLATION_CATEGORY_GPS, 15.0f, 80.0f, 26.0f, GPS_L1_HZ },
    { 7, ConstellationCategory::CONSTELLATION_CATEGORY_GALILEO, 55.0f, 160.0f, 37.0f, GPS_L1_HZ },
    { 19, ConstellationCategory::CONSTELLATION_CATEGORY_GALILEO, 30.0f, 250.0f, 33.0f, GPS_L1_HZ },
};
constexpr int SKY_SIZE = sizeof(SKY) / sizeof(SKY[0]);
/* Without a fix the receiver still "sees" the sky, too weakly to use it. */
constexpr float NO_FIX_CN0_DB = 12.0f;

Fix ReadFix()
{
    char text[PARAM_MAX] = {0};
    Fix fix;
    if (GetParameter(LOCATION_PARAM, "", text, sizeof(text)) <= 0) {
        return fix;
    }
    double v[6] = {0};
    int count = 0;
    char *p = text;
    while (count < 6 && *p != '\0') {
        while (*p == ' ' || *p == ',' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *end = nullptr;
        double d = strtod(p, &end);
        if (end == p) {
            break;
        }
        v[count++] = d;
        p = end;
    }
    if (count < 2 || std::fabs(v[0]) > 90.0 || std::fabs(v[1]) > 180.0) {
        return fix;
    }
    fix.valid = true;
    fix.latitude = v[0];
    fix.longitude = v[1];
    fix.hasAltitude = count > 2;
    fix.altitude = v[2];
    fix.hasSpeed = count > 3;
    fix.speed = static_cast<float>(v[3]);
    fix.hasBearing = count > 4;
    fix.bearing = static_cast<float>(std::fmod(std::fmod(v[4], 360.0) + 360.0, 360.0));
    if (count > 5 && v[5] > 0.0) {
        fix.accuracy = static_cast<float>(v[5]);
    }
    return fix;
}

int64_t RealtimeMs()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

uint64_t BoottimeNs()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

/* "ddmm.mmmm,N" / "dddmm.mmmm,E" as NMEA wants them. */
void FormatCoord(char *out, size_t len, double deg, bool isLat)
{
    char hemi = isLat ? (deg >= 0 ? 'N' : 'S') : (deg >= 0 ? 'E' : 'W');
    double a = std::fabs(deg);
    int d = static_cast<int>(a);
    double m = (a - d) * 60.0;
    snprintf(out, len, isLat ? "%02d%07.4f,%c" : "%03d%07.4f,%c", d, m, hemi);
}

int FinishNmea(char *buf, size_t len)
{
    unsigned char sum = 0;
    for (const char *c = buf + 1; *c != '\0'; c++) {
        sum ^= static_cast<unsigned char>(*c);
    }
    size_t used = strlen(buf);
    snprintf(buf + used, len - used, "*%02X\r\n", sum);
    return static_cast<int>(strlen(buf));
}

/*
 * All callbacks run on the receiver's own thread, never inside a vendor call:
 * the HDI calls enableGnss with the same mutex held that its callbacks take
 * (gnss_interface_impl.cpp), so a callback from inside enableGnss deadlocks
 * location_host.
 */
class Receiver {
public:
    static Receiver &Get()
    {
        static Receiver instance;
        return instance;
    }

    int Enable(const GnssCallbackStruct &callbacks)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb_ = callbacks.gnssCallback;
        enabled_ = true;
        events_.push_back({ Event::STATUS, static_cast<uint16_t>(GnssWorkingStatus::GNSS_WORKING_STATUS_POWER_ON) });
        events_.push_back({ Event::CAPABILITIES, 0 });
        if (!threadStarted_) {
            threadStarted_ = true;
            std::thread(&Receiver::Loop, this).detach();
        }
        wake_.notify_all();
        EG_LOGI("enabled");
        return VENDOR_OK;
    }

    void Disable()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            running_ = false;
            events_.push_back({ Event::STATUS,
                static_cast<uint16_t>(GnssWorkingStatus::GNSS_WORKING_STATUS_NAVIGATING_END) });
        }
        if (enabled_) {
            enabled_ = false;
            events_.push_back({ Event::STATUS, static_cast<uint16_t>(GnssWorkingStatus::GNSS_WORKING_STATUS_POWER_OFF) });
            events_.push_back({ Event::CLEAR_CALLBACKS, 0 });
        }
        wake_.notify_all();
        EG_LOGI("disabled");
    }

    int Start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) {
            return VENDOR_FAIL;
        }
        if (!running_) {
            running_ = true;
            nextReport_ = std::chrono::steady_clock::now();
            events_.push_back({ Event::STATUS,
                static_cast<uint16_t>(GnssWorkingStatus::GNSS_WORKING_STATUS_NAVIGATING_BEGIN) });
            wake_.notify_all();
            EG_LOGI("session started, interval %{public}u ms", intervalMs_);
        }
        return VENDOR_OK;
    }

    int Stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            running_ = false;
            events_.push_back({ Event::STATUS,
                static_cast<uint16_t>(GnssWorkingStatus::GNSS_WORKING_STATUS_NAVIGATING_END) });
            wake_.notify_all();
            EG_LOGI("session stopped");
        }
        return VENDOR_OK;
    }

    void SetInterval(uint32_t ms)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        intervalMs_ = ms == 0 ? DEFAULT_INTERVAL_MS : (ms < MIN_INTERVAL_MS ? MIN_INTERVAL_MS : ms);
        wake_.notify_all();
    }

private:
    struct Event {
        enum Type { STATUS, CAPABILITIES, CLEAR_CALLBACKS } type;
        uint16_t value;
    };

    void Report(const GnssBasicCallbackIfaces &cb, const Fix &fix)
    {
        int64_t nowMs = RealtimeMs();
        if (cb.satelliteStatusUpdate != nullptr) {
            GnssSatelliteStatus sats = {};
            sats.size = sizeof(sats);
            sats.satellitesNum = SKY_SIZE;
            for (int i = 0; i < SKY_SIZE; i++) {
                SatelliteStatusInfo &s = sats.satellitesList[i];
                s.size = sizeof(s);
                s.satelliteId = SKY[i].id;
                s.constellationCategory = static_cast<uint8_t>(SKY[i].constellation);
                s.cn0 = fix.valid ? SKY[i].cn0 : NO_FIX_CN0_DB;
                s.elevation = SKY[i].elevation;
                s.azimuth = SKY[i].azimuth;
                s.carrierFrequency = SKY[i].carrierHz;
                s.satelliteAdditionalInfo =
                    static_cast<uint32_t>(SatelliteAdditionalInfo::SATELLITES_ADDITIONAL_INFO_CARRIER_FREQUENCY_EXIST) |
                    (fix.valid ? static_cast<uint32_t>(SatelliteAdditionalInfo::SATELLITES_ADDITIONAL_INFO_USED_IN_FIX) |
                     static_cast<uint32_t>(SatelliteAdditionalInfo::SATELLITES_ADDITIONAL_INFO_EPHEMERIS_DATA_EXIST) : 0);
            }
            cb.satelliteStatusUpdate(&sats);
        }
        if (cb.nmeaUpdate != nullptr) {
            SendNmea(cb, fix, nowMs);
        }
        if (fix.valid && cb.locationUpdate != nullptr) {
            GnssLocation loc = {};
            loc.size = sizeof(loc);
            uint32_t v = static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_LAT_VALID) |
                static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_LONG_VALID) |
                static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_HORIZONTAL_ACCURACY_VALID) |
                static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_TIME_VALID) |
                static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_TIME_SINCE_BOOT_VALID);
            if (fix.hasAltitude) {
                v |= static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_ALTITUDE_VALID) |
                    static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_VERTICAL_ACCURACY_VALID);
            }
            if (fix.hasSpeed) {
                v |= static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_SPEED_VALID);
            }
            if (fix.hasBearing) {
                v |= static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_BEARING_VALID);
            }
            loc.fieldValidity = v;
            loc.latitude = fix.latitude;
            loc.longitude = fix.longitude;
            loc.altitude = fix.altitude;
            loc.speed = fix.speed;
            loc.bearing = fix.bearing;
            loc.horizontalAccuracy = fix.accuracy;
            loc.verticalAccuracy = fix.accuracy * 1.5f;
            loc.timeForFix = nowMs;
            loc.timeSinceBoot = BoottimeNs();
            cb.locationUpdate(&loc);
        }
    }

    void SendNmea(const GnssBasicCallbackIfaces &cb, const Fix &fix, int64_t nowMs)
    {
        time_t secs = static_cast<time_t>(nowMs / 1000);
        struct tm utc = {};
        gmtime_r(&secs, &utc);
        int cs = static_cast<int>((nowMs % 1000) / 10);
        char lat[24] = ",";
        char lon[24] = ",";
        if (fix.valid) {
            FormatCoord(lat, sizeof(lat), fix.latitude, true);
            FormatCoord(lon, sizeof(lon), fix.longitude, false);
        }
        char buf[NMEA_MAX];
        snprintf(buf, sizeof(buf), "$GPGGA,%02d%02d%02d.%02d,%s,%s,%d,%02d,%.1f,%.1f,M,0.0,M,,",
                 utc.tm_hour, utc.tm_min, utc.tm_sec, cs, lat, lon, fix.valid ? 1 : 0,
                 fix.valid ? SKY_SIZE : 0, fix.valid ? 0.9 : 99.9, fix.valid ? fix.altitude : 0.0);
        int len = FinishNmea(buf, sizeof(buf));
        cb.nmeaUpdate(nowMs, buf, len);
        snprintf(buf, sizeof(buf), "$GPRMC,%02d%02d%02d.%02d,%c,%s,%s,%.1f,%.1f,%02d%02d%02d,,,%c",
                 utc.tm_hour, utc.tm_min, utc.tm_sec, cs, fix.valid ? 'A' : 'V', lat, lon,
                 fix.valid ? fix.speed * 1.943844 : 0.0, fix.valid ? fix.bearing : 0.0,
                 utc.tm_mday, utc.tm_mon + 1, utc.tm_year % 100, fix.valid ? 'A' : 'N');
        len = FinishNmea(buf, sizeof(buf));
        cb.nmeaUpdate(nowMs, buf, len);
    }

    void Loop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (!events_.empty()) {
                Event e = events_.front();
                events_.pop_front();
                GnssBasicCallbackIfaces cb = cb_;
                if (e.type == Event::CLEAR_CALLBACKS) {
                    cb_ = {};
                    continue;
                }
                lock.unlock();
                if (e.type == Event::STATUS && cb.gnssWorkingStatusUpdate != nullptr) {
                    uint16_t value = e.value;
                    cb.gnssWorkingStatusUpdate(&value);
                } else if (e.type == Event::CAPABILITIES && cb.capabilitiesUpdate != nullptr) {
                    cb.capabilitiesUpdate(0);
                }
                lock.lock();
                continue;
            }
            auto now = std::chrono::steady_clock::now();
            if (running_ && now >= nextReport_) {
                nextReport_ = now + std::chrono::milliseconds(intervalMs_);
                GnssBasicCallbackIfaces cb = cb_;
                lock.unlock();
                Report(cb, ReadFix());
                lock.lock();
                continue;
            }
            if (running_) {
                wake_.wait_until(lock, nextReport_);
            } else {
                wake_.wait(lock);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Event> events_;
    GnssBasicCallbackIfaces cb_ = {};
    bool enabled_ = false;
    bool running_ = false;
    bool threadStarted_ = false;
    uint32_t intervalMs_ = DEFAULT_INTERVAL_MS;
    std::chrono::steady_clock::time_point nextReport_;
};

int EnableGnss(GnssCallbackStruct *callbacks)
{
    if (callbacks == nullptr) {
        return VENDOR_FAIL;
    }
    return Receiver::Get().Enable(*callbacks);
}

int DisableGnss(void)
{
    Receiver::Get().Disable();
    return VENDOR_OK;
}

int StartGnss(uint32_t category)
{
    if (category != static_cast<uint32_t>(GnssStartCategory::GNSS_START_CATEGORY_NORMAL)) {
        EG_LOGW("start category %{public}u not supported", category);
        return VENDOR_FAIL;
    }
    return Receiver::Get().Start();
}

int StopGnss(uint32_t category)
{
    if (category != static_cast<uint32_t>(GnssStartCategory::GNSS_START_CATEGORY_NORMAL)) {
        return VENDOR_OK;
    }
    return Receiver::Get().Stop();
}

/* The position comes from the parameter; reference time and location from
 * the framework are accepted and ignored. */
int InjectsGnssReferenceInfo(int category, GnssReferenceInfo *info)
{
    (void)category;
    return info == nullptr ? VENDOR_FAIL : VENDOR_OK;
}

int SetGnssConfigPara(GnssConfigParameter *para)
{
    if (para == nullptr ||
        para->startCategory != static_cast<uint32_t>(GnssStartCategory::GNSS_START_CATEGORY_NORMAL)) {
        return VENDOR_FAIL;
    }
    Receiver::Get().SetInterval(para->u.gnssBasicConfig.minInterval);
    return VENDOR_OK;
}

void RemoveAuxiliaryData(uint16_t flags)
{
    (void)flags;
}

int InjectExtendedEphemeris(char *data, int length)
{
    (void)data;
    (void)length;
    return VENDOR_FAIL;
}

int GetCachedLocationsSize()
{
    return 0;
}

void FlushCachedGnssLocations()
{
}

/* No AGNSS, geofencing, NI or measurements: every HDI caller handles NULL. */
const void *GetGnssModuleIface(int iface)
{
    (void)iface;
    return nullptr;
}

const GnssVendorInterface g_vendorInterface = {
    .size = sizeof(GnssVendorInterface),
    .enableGnss = EnableGnss,
    .disableGnss = DisableGnss,
    .startGnss = StartGnss,
    .stopGnss = StopGnss,
    .injectsGnssReferenceInfo = InjectsGnssReferenceInfo,
    .setGnssConfigPara = SetGnssConfigPara,
    .removeAuxiliaryData = RemoveAuxiliaryData,
    .injectExtendedEphemeris = InjectExtendedEphemeris,
    .getCachedLocationsSize = GetCachedLocationsSize,
    .flushCachedGnssLocations = FlushCachedGnssLocations,
    .getGnssModuleIface = GetGnssModuleIface,
};

const GnssVendorInterface *GetGnssInterface()
{
    return &g_vendorInterface;
}

} // namespace
} // namespace Location
} // namespace HDI
} // namespace OHOS

extern "C" {
__attribute__((visibility("default")))
OHOS::HDI::Location::GnssVendorDevice GnssVendorInterface = {
    .size = sizeof(OHOS::HDI::Location::GnssVendorDevice),
    .getGnssInterface = OHOS::HDI::Location::GetGnssInterface,
};
}
