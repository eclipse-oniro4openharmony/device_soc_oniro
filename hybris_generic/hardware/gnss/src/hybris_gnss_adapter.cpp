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
 * vendorGnssAdapter.so — the whole OHOS GNSS vendor ABI.
 *
 * location_host's GNSS, AGNSS and geofence HDIs each dlopen this library by
 * that exact name and dlsym one object, `GnssVendorInterface`, of type
 * GnssVendorDevice (drivers/peripheral/location/gnss/hdi_service/
 * location_vendor_interface.cpp).  Everything behind it is forwarded to the
 * Halium container's android.hardware.gnss.IGnss/default AIDL v3 HAL.
 */

#include <cstring>

#include "hybris_gnss_bridge.h"
#include "hybris_gnss_log.h"

namespace OHOS {
namespace HybrisGnss {
namespace {

/* Long enough for a HAL that is merely slow to register, short enough that
 * the framework's hdiMutex_ is not held for long on a cold boot.  A session
 * asked for before the HAL is up is replayed on connect anyway. */
constexpr int32_t ENABLE_CONNECT_WAIT_MS = 5000;
constexpr int32_t INJECT_CONNECT_WAIT_MS = 1000;

constexpr int VENDOR_OK = 0;
constexpr int VENDOR_FAIL = -1;

gnss::GnssLocation ToAidlLocation(const GnssLocation &in)
{
    gnss::GnssLocation out;
    int32_t flags = 0;
    auto has = [&in](GnssLocationValidity bit) {
        return (in.fieldValidity & static_cast<uint32_t>(bit)) != 0;
    };
    if (has(GnssLocationValidity::GNSS_LOCATION_LAT_VALID) &&
        has(GnssLocationValidity::GNSS_LOCATION_LONG_VALID)) {
        flags |= gnss::GnssLocation::HAS_LAT_LONG;
    }
    if (has(GnssLocationValidity::GNSS_LOCATION_ALTITUDE_VALID)) {
        flags |= gnss::GnssLocation::HAS_ALTITUDE;
    }
    if (has(GnssLocationValidity::GNSS_LOCATION_SPEED_VALID)) {
        flags |= gnss::GnssLocation::HAS_SPEED;
    }
    if (has(GnssLocationValidity::GNSS_LOCATION_BEARING_VALID)) {
        flags |= gnss::GnssLocation::HAS_BEARING;
    }
    if (has(GnssLocationValidity::GNSS_LOCATION_HORIZONTAL_ACCURACY_VALID)) {
        flags |= gnss::GnssLocation::HAS_HORIZONTAL_ACCURACY;
    }
    if (has(GnssLocationValidity::GNSS_LOCATION_VERTICAL_ACCURACY_VALID)) {
        flags |= gnss::GnssLocation::HAS_VERTICAL_ACCURACY;
    }
    if (has(GnssLocationValidity::GNSS_LOCATION_SPEED_ACCURACY_VALID)) {
        flags |= gnss::GnssLocation::HAS_SPEED_ACCURACY;
    }
    if (has(GnssLocationValidity::GNSS_LOCATION_BEARING_ACCURACY_VALID)) {
        flags |= gnss::GnssLocation::HAS_BEARING_ACCURACY;
    }
    /* The framework's network/cached fixes arrive with no validity bits at
     * all; a location with a position and an accuracy is still usable. */
    if (in.fieldValidity == 0 && (in.latitude != 0.0 || in.longitude != 0.0)) {
        flags |= gnss::GnssLocation::HAS_LAT_LONG;
        if (in.horizontalAccuracy > 0.0f) {
            flags |= gnss::GnssLocation::HAS_HORIZONTAL_ACCURACY;
        }
    }
    out.gnssLocationFlags = flags;
    out.latitudeDegrees = in.latitude;
    out.longitudeDegrees = in.longitude;
    out.altitudeMeters = in.altitude;
    out.speedMetersPerSec = in.speed;
    out.bearingDegrees = in.bearing;
    out.horizontalAccuracyMeters = in.horizontalAccuracy;
    out.verticalAccuracyMeters = in.verticalAccuracy;
    out.speedAccuracyMetersPerSecond = in.speedAccuracy;
    out.bearingAccuracyDegrees = in.bearingAccuracy;
    out.timestampMillis = in.timeForFix;
    if (in.timeSinceBoot != 0) {
        out.elapsedRealtime.flags = gnss::ElapsedRealtime::HAS_TIMESTAMP_NS;
        out.elapsedRealtime.timestampNs = static_cast<int64_t>(in.timeSinceBoot);
        if (in.timeUncertainty != 0) {
            out.elapsedRealtime.flags |= gnss::ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS;
            out.elapsedRealtime.timeUncertaintyNs = static_cast<double>(in.timeUncertainty);
        }
    }
    return out;
}

int EnableGnss(GnssCallbackStruct *callbacks)
{
    if (callbacks == nullptr) {
        return VENDOR_FAIL;
    }
    GnssBridge &bridge = GnssBridge::Get();
    bridge.SetCallbacks(*callbacks);
    if (!bridge.WaitConnected(ENABLE_CONNECT_WAIT_MS)) {
        if (bridge.Unavailable()) {
            HG_LOGE("no GNSS HAL on this device");
            return VENDOR_FAIL;
        }
        /* Still connecting: accept, and let the connect replay the session. */
        HG_LOGW("HAL not reachable yet — enable deferred to connect");
    }
    HG_LOGI("enabled");
    return VENDOR_OK;
}

int DisableGnss(void)
{
    GnssBridge::Get().Disable();
    HG_LOGI("disabled");
    return VENDOR_OK;
}

int StartGnss(uint32_t category)
{
    if (category != static_cast<uint32_t>(GnssStartCategory::GNSS_START_CATEGORY_NORMAL)) {
        /* GNSS_CACHE is batching, which only wearables ask for
         * (GnssAbility::IsSupportBatching) — not supported here. */
        HG_LOGW("start category %{public}u not supported", category);
        return VENDOR_FAIL;
    }
    return GnssBridge::Get().StartSession();
}

int StopGnss(uint32_t category)
{
    if (category != static_cast<uint32_t>(GnssStartCategory::GNSS_START_CATEGORY_NORMAL)) {
        return VENDOR_OK;
    }
    return GnssBridge::Get().StopSession();
}

int InjectsGnssReferenceInfo(int category, GnssReferenceInfo *info)
{
    if (info == nullptr) {
        return VENDOR_FAIL;
    }
    (void)GnssBridge::Get().WaitConnected(INJECT_CONNECT_WAIT_MS);
    auto hal = GnssBridge::Get().Gnss();
    if (hal == nullptr) {
        return VENDOR_FAIL;
    }
    ::ndk::ScopedAStatus st;
    switch (static_cast<GnssRefInfoCategory>(category)) {
        case GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_TIME:
            st = hal->injectTime(info->u.time.time, info->u.time.elapsedRealtime,
                                 info->u.time.uncertaintyOfTime);
            break;
        case GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_GNSS_LOCATION:
        case GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_GNSS_LOCATION_EMERGENCY:
            st = hal->injectLocation(ToAidlLocation(info->u.gnssLocation));
            break;
        case GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_BEST_LOCATION:
        case GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_BEST_LOCATION_EMERGENCY:
            st = hal->injectBestLocation(ToAidlLocation(info->u.bestLocation));
            break;
        default:
            return VENDOR_FAIL;
    }
    if (!st.isOk()) {
        HG_LOGW("inject category %{public}d: %{public}s", category, st.getDescription().c_str());
        return VENDOR_FAIL;
    }
    HG_LOGI("injected reference info category %{public}d", category);
    return VENDOR_OK;
}

int SetGnssConfigPara(GnssConfigParameter *para)
{
    if (para == nullptr) {
        return VENDOR_FAIL;
    }
    if (para->startCategory != static_cast<uint32_t>(GnssStartCategory::GNSS_START_CATEGORY_NORMAL)) {
        return VENDOR_FAIL;
    }
    GnssBridge::Get().SetPositionConfig(para->u.gnssBasicConfig.gnssMode,
                                        para->u.gnssBasicConfig.minInterval);
    return VENDOR_OK;
}

/* GnssAuxiliaryDataCategory matches IGnss.GnssAidingData bit for bit, except
 * CELLDB_INFO (1 << 11 here, 0x8000 there). */
void RemoveAuxiliaryData(uint16_t flags)
{
    auto hal = GnssBridge::Get().Gnss();
    if (hal == nullptr) {
        return;
    }
    int32_t aidl;
    if (flags == static_cast<uint16_t>(GnssAuxiliaryDataCategory::GNSS_AUXILIARY_DATA_CATEGORY_ALL)) {
        aidl = static_cast<int32_t>(gnss::IGnss::GnssAidingData::ALL);
    } else {
        constexpr uint16_t CELLDB = static_cast<uint16_t>(
            GnssAuxiliaryDataCategory::GNSS_AUXILIARY_DATA_CATEGORY_CELLDB_INFO);
        aidl = flags & ~CELLDB & 0x07FF;
        if (flags & CELLDB) {
            aidl |= static_cast<int32_t>(gnss::IGnss::GnssAidingData::CELLDB_INFO);
        }
    }
    auto st = hal->deleteAidingData(static_cast<gnss::IGnss::GnssAidingData>(aidl));
    if (!st.isOk()) {
        HG_LOGW("IGnss.deleteAidingData: %{public}s", st.getDescription().c_str());
        return;
    }
    HG_LOGI("deleted aiding data 0x%{public}x", aidl);
}

/* PSDS (MTK's EPO) is downloaded by the vendor's own lbs service inside the
 * container; the framework never calls this (SetPredictGnssData is a no-op
 * in the stock HDI). */
int InjectExtendedEphemeris(char *data, int length)
{
    (void)data;
    (void)length;
    return VENDOR_FAIL;
}

/* Batching is a wearable-only feature in the framework. */
int GetCachedLocationsSize()
{
    return 0;
}

void FlushCachedGnssLocations()
{
}

const void *GetGnssModuleIface(int iface)
{
    switch (static_cast<GnssModuleIfaceCategory>(iface)) {
        case GnssModuleIfaceCategory::AGNSS_MODULE_INTERFACE:
            return AgnssModule();
        default:
            /* Geofencing, network-initiated and measurement modules are not
             * bridged; every HDI caller handles NULL as "not supported". */
            return nullptr;
    }
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

/*
 * Called once per HDI device that loads us (GNSS, AGNSS, geofence — all in
 * location_host, all sharing this one copy).  The connect starts here, so
 * the HAL is usually bound by the time the framework's first enableGnss
 * arrives.
 */
const GnssVendorInterface *GetGnssInterface()
{
    GnssBridge::Get().Start();
    return &g_vendorInterface;
}

} // namespace
} // namespace HybrisGnss
} // namespace OHOS

extern "C" {
__attribute__((visibility("default")))
OHOS::HDI::Location::GnssVendorDevice GnssVendorInterface = {
    .size = sizeof(OHOS::HDI::Location::GnssVendorDevice),
    .getGnssInterface = OHOS::HybrisGnss::GetGnssInterface,
};
}
