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

/*
 * The HAL → OHOS direction.  Each callback converts the AIDL parcelable into
 * the location_vendor_lib.h struct on the binder thread it arrived on, then
 * hands the OHOS callback to the bridge's dispatcher (see GnssBridge::Post
 * for why it must not be called inline).
 */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include <aidl/android/hardware/gnss/BnAGnssCallback.h>
#include <aidl/android/hardware/gnss/BnAGnssRilCallback.h>
#include <aidl/android/hardware/gnss/BnGnssCallback.h>

#include "hybris_gnss_bridge.h"
#include "hybris_gnss_log.h"

namespace OHOS {
namespace HybrisGnss {
namespace {

using ::ndk::ScopedAStatus;

uint32_t ToOhosValidity(int32_t flags)
{
    uint32_t v = 0;
    auto set = [&v](bool on, GnssLocationValidity bit) {
        if (on) {
            v |= static_cast<uint32_t>(bit);
        }
    };
    set(flags & gnss::GnssLocation::HAS_LAT_LONG, GnssLocationValidity::GNSS_LOCATION_LAT_VALID);
    set(flags & gnss::GnssLocation::HAS_LAT_LONG, GnssLocationValidity::GNSS_LOCATION_LONG_VALID);
    set(flags & gnss::GnssLocation::HAS_ALTITUDE, GnssLocationValidity::GNSS_LOCATION_ALTITUDE_VALID);
    set(flags & gnss::GnssLocation::HAS_SPEED, GnssLocationValidity::GNSS_LOCATION_SPEED_VALID);
    set(flags & gnss::GnssLocation::HAS_BEARING, GnssLocationValidity::GNSS_LOCATION_BEARING_VALID);
    set(flags & gnss::GnssLocation::HAS_HORIZONTAL_ACCURACY,
        GnssLocationValidity::GNSS_LOCATION_HORIZONTAL_ACCURACY_VALID);
    set(flags & gnss::GnssLocation::HAS_VERTICAL_ACCURACY,
        GnssLocationValidity::GNSS_LOCATION_VERTICAL_ACCURACY_VALID);
    set(flags & gnss::GnssLocation::HAS_SPEED_ACCURACY,
        GnssLocationValidity::GNSS_LOCATION_SPEED_ACCURACY_VALID);
    set(flags & gnss::GnssLocation::HAS_BEARING_ACCURACY,
        GnssLocationValidity::GNSS_LOCATION_BEARING_ACCURACY_VALID);
    return v;
}

/* The GnssCapabilities bits OHOS knows about, out of IGnssCallback's. */
uint32_t ToOhosCapabilities(int32_t caps)
{
    uint32_t v = 0;
    auto map = [&v, caps](int32_t aidl, GnssCapabilities ohos) {
        if (caps & aidl) {
            v |= static_cast<uint32_t>(ohos);
        }
    };
    map(gnss::IGnssCallback::CAPABILITY_MSB, GnssCapabilities::GNSS_CAP_SUPPORT_MSB);
    map(gnss::IGnssCallback::CAPABILITY_MSA, GnssCapabilities::GNSS_CAP_SUPPORT_MSA);
    map(gnss::IGnssCallback::CAPABILITY_GEOFENCING, GnssCapabilities::GNSS_CAP_SUPPORT_GEOFENCING);
    map(gnss::IGnssCallback::CAPABILITY_MEASUREMENTS, GnssCapabilities::GNSS_CAP_SUPPORT_MEASUREMENTS);
    map(gnss::IGnssCallback::CAPABILITY_NAV_MESSAGES, GnssCapabilities::GNSS_CAP_SUPPORT_NAV_MESSAGES);
    return v;
}

class GnssCallback : public gnss::BnGnssCallback {
public:
    ScopedAStatus gnssSetCapabilitiesCb(int32_t capabilities) override
    {
        HG_LOGI("HAL capabilities 0x%{public}x", capabilities);
        uint32_t ohos = ToOhosCapabilities(capabilities);
        GnssBridge::Get().Post([ohos]() {
            auto cb = GnssBridge::Get().Callbacks().gnssCallback.capabilitiesUpdate;
            if (cb != nullptr) {
                cb(ohos);
            }
        });
        return ScopedAStatus::ok();
    }

    /* GnssStatusValue and GnssWorkingStatus share their numbering
     * (SESSION_BEGIN/END = NAVIGATING_BEGIN/END, ENGINE_ON/OFF = POWER_ON/OFF). */
    ScopedAStatus gnssStatusCb(gnss::IGnssCallback::GnssStatusValue status) override
    {
        uint16_t value = static_cast<uint16_t>(status);
        HG_LOGI("HAL status %{public}u", value);
        GnssBridge::Get().Post([value]() {
            auto cb = GnssBridge::Get().Callbacks().gnssCallback.gnssWorkingStatusUpdate;
            if (cb != nullptr) {
                uint16_t v = value;
                cb(&v);
            }
        });
        return ScopedAStatus::ok();
    }

    ScopedAStatus gnssSvStatusCb(const std::vector<gnss::IGnssCallback::GnssSvInfo> &list) override
    {
        auto status = std::make_shared<GnssSatelliteStatus>();
        (void)memset(status.get(), 0, sizeof(*status));
        status->size = sizeof(GnssSatelliteStatus);
        size_t n = std::min<size_t>(list.size(), SATELLITE_NUM_MAXIMUM);
        status->satellitesNum = static_cast<uint32_t>(n);
        size_t used = 0;
        for (size_t i = 0; i < n; i++) {
            const auto &in = list[i];
            SatelliteStatusInfo &out = status->satellitesList[i];
            out.size = sizeof(SatelliteStatusInfo);
            out.satelliteId = static_cast<int16_t>(in.svid);
            /* GnssConstellationType and ConstellationCategory share values. */
            out.constellationCategory = static_cast<uint8_t>(in.constellation);
            out.cn0 = in.cN0Dbhz;
            out.elevation = in.elevationDegrees;
            out.azimuth = in.azimuthDegrees;
            out.carrierFrequency = static_cast<float>(in.carrierFrequencyHz);
            /* GnssSvFlags and SatelliteAdditionalInfo share their bits. */
            out.satelliteAdditionalInfo = static_cast<uint32_t>(in.svFlag);
            if (in.svFlag & static_cast<int32_t>(gnss::IGnssCallback::GnssSvFlags::USED_IN_FIX)) {
                used++;
            }
        }
        LogSvSummary(n, used);
        GnssBridge::Get().Post([status]() {
            auto cb = GnssBridge::Get().Callbacks().gnssCallback.satelliteStatusUpdate;
            if (cb != nullptr) {
                cb(status.get());
            }
        });
        return ScopedAStatus::ok();
    }

    ScopedAStatus gnssLocationCb(const gnss::GnssLocation &in) override
    {
        auto loc = std::make_shared<GnssLocation>();
        (void)memset(loc.get(), 0, sizeof(*loc));
        loc->size = sizeof(GnssLocation);
        loc->fieldValidity = ToOhosValidity(in.gnssLocationFlags);
        loc->latitude = in.latitudeDegrees;
        loc->longitude = in.longitudeDegrees;
        loc->altitude = in.altitudeMeters;
        loc->speed = static_cast<float>(in.speedMetersPerSec);
        loc->bearing = static_cast<float>(in.bearingDegrees);
        loc->horizontalAccuracy = static_cast<float>(in.horizontalAccuracyMeters);
        loc->verticalAccuracy = static_cast<float>(in.verticalAccuracyMeters);
        loc->speedAccuracy = static_cast<float>(in.speedAccuracyMetersPerSecond);
        loc->bearingAccuracy = static_cast<float>(in.bearingAccuracyDegrees);
        loc->timeForFix = in.timestampMillis;
        loc->fieldValidity |= static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_TIME_VALID);
        /* Same kernel, same CLOCK_BOOTTIME: the container's elapsed-realtime
         * stamp is directly an OHOS time-since-boot. */
        if (in.elapsedRealtime.flags & gnss::ElapsedRealtime::HAS_TIMESTAMP_NS) {
            loc->timeSinceBoot = static_cast<uint64_t>(in.elapsedRealtime.timestampNs);
            loc->fieldValidity |=
                static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_TIME_SINCE_BOOT_VALID);
        }
        if (in.elapsedRealtime.flags & gnss::ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS) {
            loc->timeUncertainty = static_cast<uint64_t>(in.elapsedRealtime.timeUncertaintyNs);
            loc->fieldValidity |=
                static_cast<uint32_t>(GnssLocationValidity::GNSS_LOCATION_TIME_UNCERTAINTY_VALID);
        }
        LogFix(*loc);
        GnssBridge::Get().Post([loc]() {
            auto cb = GnssBridge::Get().Callbacks().gnssCallback.locationUpdate;
            if (cb != nullptr) {
                cb(loc.get());
            }
        });
        return ScopedAStatus::ok();
    }

    ScopedAStatus gnssNmeaCb(int64_t timestamp, const std::string &nmea) override
    {
        auto copy = std::make_shared<std::string>(nmea);
        GnssBridge::Get().Post([timestamp, copy]() {
            auto cb = GnssBridge::Get().Callbacks().gnssCallback.nmeaUpdate;
            if (cb != nullptr) {
                cb(timestamp, copy->c_str(), static_cast<int>(copy->size()));
            }
        });
        return ScopedAStatus::ok();
    }

    /* The OHOS HDI has no wakelock seam; the HAL only asks so the AP stays
     * up across a report, and nothing here suspends mid-callback. */
    ScopedAStatus gnssAcquireWakelockCb() override
    {
        return ScopedAStatus::ok();
    }

    ScopedAStatus gnssReleaseWakelockCb() override
    {
        return ScopedAStatus::ok();
    }

    ScopedAStatus gnssSetSystemInfoCb(const gnss::IGnssCallback::GnssSystemInfo &info) override
    {
        HG_LOGI("HAL system info: %{public}s (%{public}d)", info.name.c_str(), info.yearOfHw);
        return ScopedAStatus::ok();
    }

    /* The location framework runs its own NTP and injects time and location
     * on its schedule (gnss_ability.cpp); these requests are forwarded for
     * completeness, and the stock HDI leaves the hook NULL today. */
    ScopedAStatus gnssRequestTimeCb() override
    {
        PostRefInfoRequest(GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_TIME);
        return ScopedAStatus::ok();
    }

    ScopedAStatus gnssRequestLocationCb(bool independentFromGnss, bool isUserEmergency) override
    {
        GnssRefInfoCategory category;
        if (isUserEmergency) {
            category = independentFromGnss ? GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_BEST_LOCATION_EMERGENCY
                                            : GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_GNSS_LOCATION_EMERGENCY;
        } else {
            category = independentFromGnss ? GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_BEST_LOCATION
                                            : GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_GNSS_LOCATION;
        }
        PostRefInfoRequest(category);
        return ScopedAStatus::ok();
    }

    ScopedAStatus gnssSetSignalTypeCapabilitiesCb(const std::vector<gnss::GnssSignalType> &types) override
    {
        HG_LOGI("HAL reports %{public}zu signal types", types.size());
        return ScopedAStatus::ok();
    }

private:
    static void PostRefInfoRequest(GnssRefInfoCategory category)
    {
        int value = static_cast<int>(category);
        HG_LOGI("HAL requests reference info %{public}d", value);
        GnssBridge::Get().Post([value]() {
            auto cb = GnssBridge::Get().Callbacks().gnssCallback.requestRefInfo;
            if (cb != nullptr) {
                cb(value);
            }
        });
    }

    /* MTK reports satellites at 1 Hz for as long as a session runs; one
     * summary line every 10 s is enough to watch a cold start converge. */
    void LogSvSummary(size_t visible, size_t used)
    {
        constexpr uint32_t EVERY = 10;
        if (svReports_++ % EVERY == 0) {
            HG_LOGI("satellites: %{public}zu visible, %{public}zu used in fix", visible, used);
        }
    }

    void LogFix(const GnssLocation &loc)
    {
        constexpr uint32_t EVERY = 30;
        uint32_t n = ++fixReports_;
        if ((n - 1) % EVERY == 0) {
            HG_LOGI("fix #%{public}u: accuracy %{public}.1f m", n, loc.horizontalAccuracy);
        }
    }

    /* Bumped from whichever binder thread delivers the report. */
    std::atomic<uint32_t> svReports_ { 0 };
    std::atomic<uint32_t> fixReports_ { 0 };
};

/*
 * AGNSS data link.  The OHOS location framework does not set one up — its
 * RequestSetUpAgnssDataLink only logs (agnss_event_callback.cpp) — while an
 * Android HAL waits for dataConnOpen/dataConnFailed after every request.
 * So the request is forwarded *and* answered here: "open" on whatever
 * network is the default (handle 0), which is how the MTK SUPL client
 * reaches its server over WiFi or mobile data alike.
 */
class AGnssCallback : public gnss::BnAGnssCallback {
public:
    ScopedAStatus agnssStatusCb(gnss::IAGnssCallback::AGnssType type,
                                gnss::IAGnssCallback::AGnssStatusValue status) override
    {
        HG_LOGI("AGNSS type %{public}d status %{public}d", static_cast<int>(type),
                static_cast<int>(status));
        uint16_t requestCategory;
        switch (status) {
            case gnss::IAGnssCallback::AGnssStatusValue::REQUEST_AGNSS_DATA_CONN:
                requestCategory =
                    static_cast<uint16_t>(AgnssDataConnectionSetUpCategory::ESTABLISH_DATA_CONNECTION);
                break;
            case gnss::IAGnssCallback::AGnssStatusValue::RELEASE_AGNSS_DATA_CONN:
                requestCategory =
                    static_cast<uint16_t>(AgnssDataConnectionSetUpCategory::RELEASE_DATA_CONNECTION);
                break;
            default:
                return ScopedAStatus::ok();
        }
        uint16_t agnssCategory = static_cast<uint16_t>(type);
        GnssBridge::Get().Post([agnssCategory, requestCategory]() {
            auto cb = GnssBridge::Get().AgnssCallbacks().requestSetupDataLink;
            if (cb != nullptr) {
                AgnssDataConnectionRequest req;
                (void)memset(&req, 0, sizeof(req));
                req.size = sizeof(req);
                req.agnssCategory = agnssCategory;
                req.requestCategory = requestCategory;
                cb(&req);
            }
            auto agnss = GnssBridge::Get().AGnss();
            if (agnss == nullptr) {
                return;
            }
            ScopedAStatus st = (requestCategory ==
                static_cast<uint16_t>(AgnssDataConnectionSetUpCategory::ESTABLISH_DATA_CONNECTION))
                ? agnss->dataConnOpen(0, "", gnss::IAGnss::ApnIpType::IPV4V6)
                : agnss->dataConnClosed();
            if (!st.isOk()) {
                HG_LOGW("AGNSS data link answer: %{public}s", st.getDescription().c_str());
            }
        });
        return ScopedAStatus::ok();
    }
};

class AGnssRilCallback : public gnss::BnAGnssRilCallback {
public:
    /* IAGnssRil's set-ID flags are 1 = IMSI, 2 = MSISDN, the same values as
     * AgnssSetidCategory. */
    ScopedAStatus requestSetIdCb(int32_t setIdFlag) override
    {
        HG_LOGI("AGNSS requests set ID 0x%{public}x", setIdFlag);
        uint16_t category = (setIdFlag & static_cast<int32_t>(AgnssSetidCategory::AGNSS_SETID_CATEGORY_IMSI))
            ? static_cast<uint16_t>(AgnssSetidCategory::AGNSS_SETID_CATEGORY_IMSI)
            : static_cast<uint16_t>(AgnssSetidCategory::AGNSS_SETID_CATEGORY_MSISDN);
        GnssBridge::Get().Post([category]() {
            auto cb = GnssBridge::Get().AgnssCallbacks().requestSetid;
            if (cb != nullptr) {
                cb(category);
            }
        });
        return ScopedAStatus::ok();
    }

    ScopedAStatus requestRefLocCb() override
    {
        HG_LOGI("AGNSS requests reference location");
        GnssBridge::Get().Post([]() {
            auto cb = GnssBridge::Get().AgnssCallbacks().requestRefInfo;
            if (cb != nullptr) {
                cb(static_cast<uint32_t>(AgnssRefInfoCategory::AGNSS_REF_INFO_CATEGORY_CELLID));
            }
        });
        return ScopedAStatus::ok();
    }
};

} // namespace

std::shared_ptr<gnss::IGnssCallback> MakeGnssCallback()
{
    return ::ndk::SharedRefBase::make<GnssCallback>();
}

std::shared_ptr<gnss::IAGnssCallback> MakeAGnssCallback()
{
    return ::ndk::SharedRefBase::make<AGnssCallback>();
}

std::shared_ptr<gnss::IAGnssRilCallback> MakeAGnssRilCallback()
{
    return ::ndk::SharedRefBase::make<AGnssRilCallback>();
}

} // namespace HybrisGnss
} // namespace OHOS
