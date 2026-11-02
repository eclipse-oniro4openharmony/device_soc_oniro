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
 * The legacy backend: MediaTek's gps.default.so, loaded into location_host
 * through libhybris, behind the same AIDL interfaces the bridge already
 * drives.
 *
 * Halium 12 MTK vendors (the Volla X23) ship the AIDL IGnss only as version
 * 1, which has no session API at all (no start, setPositionMode, AGnss...);
 * positions are served over HIDL gnss@2.1, which is C++-only and so out of
 * reach from OHOS.  Both of those services are thin wrappers around the
 * legacy HAL module, which talks to mnld over abstract sockets in the shared
 * network namespace.  So we load the module ourselves and present it as an
 * in-process gnss::IGnss / IAGnss / IAGnssRil: GnssBridge, the vendor ABI in
 * hybris_gnss_adapter.cpp and the AGNSS module keep working unchanged, and
 * the HAL's C callbacks are fed into the very IGnssCallback objects the AIDL
 * path hands to the container.
 *
 * mnld serves one HAL client, so the device's init cfg must keep the
 * container's own GNSS service from starting (init.x23.cfg masks its .rc).
 */

#include "hybris_gnss_legacy.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <pthread.h>

#include "hybris_gnss_log.h"
#include "hybris_gps_legacy_abi.h"

namespace OHOS {
namespace HybrisGnss {
namespace {

using ::ndk::ScopedAStatus;

/* ── loading ── */

using HwGetModuleFn = int (*)(const char *id, const struct hw_module_t **module);

const GpsInterface_ext *OpenLegacyHal()
{
    /* libhybris' libhardware: a wrapper that runs Android's hw_get_module
     * under the hybris linker (the audio VDI loads it the same way). */
    static const char *paths[] = {
        "libhardware.z.so",
        "/system/lib64/libhardware.z.so",
        "libhardware.so",
    };
    void *lib = nullptr;
    for (const char *p : paths) {
        lib = dlopen(p, RTLD_NOW);
        if (lib != nullptr) {
            break;
        }
    }
    if (lib == nullptr) {
        HG_LOGE("dlopen(libhardware) failed: %{public}s", dlerror());
        return nullptr;
    }
    auto getModule = reinterpret_cast<HwGetModuleFn>(dlsym(lib, "hw_get_module"));
    if (getModule == nullptr) {
        HG_LOGE("libhardware has no hw_get_module");
        return nullptr;
    }

    const struct hw_module_t *module = nullptr;
    int err = getModule(HYBRIS_GPS_HARDWARE_MODULE_ID, &module);
    if (err != 0 || module == nullptr || module->methods == nullptr || module->methods->open == nullptr) {
        HG_LOGE("hw_get_module(gps) failed: %{public}d", err);
        return nullptr;
    }
    struct hw_device_t *device = nullptr;
    err = module->methods->open(module, HYBRIS_GPS_HARDWARE_MODULE_ID, &device);
    if (err != 0 || device == nullptr) {
        HG_LOGE("gps module open failed: %{public}d", err);
        return nullptr;
    }
    auto *gpsDevice = reinterpret_cast<struct gps_device_t *>(device);
    const GpsInterface_ext *gps = gpsDevice->get_gps_interface(gpsDevice);
    if (gps == nullptr) {
        HG_LOGE("gps module has no GpsInterface");
        return nullptr;
    }
    HG_LOGI("legacy GPS HAL %{public}s (%{public}s) loaded, GpsInterface size %{public}zu",
            module->name != nullptr ? module->name : "?", module->author != nullptr ? module->author : "?",
            gps->size);
    return gps;
}

/* ── process-wide state: the HAL's callbacks carry no context ── */

struct LegacyState {
    std::mutex lock;
    std::shared_ptr<gnss::IGnssCallback> gnssCb;
    std::shared_ptr<gnss::IAGnssCallback> agnssCb;
    std::shared_ptr<gnss::IAGnssRilCallback> rilCb;
};

LegacyState &State()
{
    static LegacyState *state = new LegacyState();
    return *state;
}

std::shared_ptr<gnss::IGnssCallback> GnssCb()
{
    std::lock_guard<std::mutex> guard(State().lock);
    return State().gnssCb;
}

std::shared_ptr<gnss::IAGnssCallback> AGnssCb()
{
    std::lock_guard<std::mutex> guard(State().lock);
    return State().agnssCb;
}

std::shared_ptr<gnss::IAGnssRilCallback> RilCb()
{
    std::lock_guard<std::mutex> guard(State().lock);
    return State().rilCb;
}

/* ── HAL → AIDL callbacks ── */

struct ThreadStart {
    void (*start)(void *);
    void *arg;
    std::string name;
};

void *ThreadTrampoline(void *opaque)
{
    auto *ts = static_cast<ThreadStart *>(opaque);
    if (!ts->name.empty()) {
        /* pthread names are capped at 15 characters. */
        (void)pthread_setname_np(pthread_self(), ts->name.substr(0, 15).c_str());
    }
    void (*start)(void *) = ts->start;
    void *arg = ts->arg;
    delete ts;
    start(arg);
    return nullptr;
}

pthread_t CreateThread(const char *name, void (*start)(void *), void *arg)
{
    auto *ts = new ThreadStart { start, arg, name != nullptr ? name : "" };
    /* The HAL was written for bionic's 1 MiB thread stacks: mnl2hal_hdlr
     * alone keeps ~75 KB of message structs on the stack, which overruns
     * musl's much smaller default and faults on the first report. */
    constexpr size_t HAL_THREAD_STACK = 1024 * 1024;
    pthread_attr_t attr;
    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setstacksize(&attr, HAL_THREAD_STACK);
    pthread_t thread {};
    int err = pthread_create(&thread, &attr, ThreadTrampoline, ts);
    (void)pthread_attr_destroy(&attr);
    if (err != 0) {
        HG_LOGE("create_thread(%{public}s): %{public}d", ts->name.c_str(), err);
        delete ts;
        return pthread_t {};
    }
    (void)pthread_detach(thread);
    return thread;
}

void LocationCb(GpsLocation_ext *in)
{
    auto cb = GnssCb();
    if (in == nullptr || cb == nullptr) {
        return;
    }
    const GpsLocation &l = in->legacyLocation;
    gnss::GnssLocation out;
    int32_t flags = 0;
    if (l.flags & GPS_LOCATION_HAS_LAT_LONG) {
        flags |= gnss::GnssLocation::HAS_LAT_LONG;
    }
    if (l.flags & GPS_LOCATION_HAS_ALTITUDE) {
        flags |= gnss::GnssLocation::HAS_ALTITUDE;
    }
    if (l.flags & GPS_LOCATION_HAS_SPEED) {
        flags |= gnss::GnssLocation::HAS_SPEED;
    }
    if (l.flags & GPS_LOCATION_HAS_BEARING) {
        flags |= gnss::GnssLocation::HAS_BEARING;
    }
    if (l.flags & GPS_LOCATION_HAS_HORIZONTAL_ACCURACY) {
        flags |= gnss::GnssLocation::HAS_HORIZONTAL_ACCURACY;
    }
    if (l.flags & GPS_LOCATION_HAS_VERTICAL_ACCURACY) {
        flags |= gnss::GnssLocation::HAS_VERTICAL_ACCURACY;
    }
    if (l.flags & GPS_LOCATION_HAS_SPEED_ACCURACY) {
        flags |= gnss::GnssLocation::HAS_SPEED_ACCURACY;
    }
    if (l.flags & GPS_LOCATION_HAS_BEARING_ACCURACY) {
        flags |= gnss::GnssLocation::HAS_BEARING_ACCURACY;
    }
    out.gnssLocationFlags = flags;
    out.latitudeDegrees = l.latitude;
    out.longitudeDegrees = l.longitude;
    out.altitudeMeters = l.altitude;
    out.speedMetersPerSec = l.speed;
    out.bearingDegrees = l.bearing;
    /* GpsLocation.accuracy is the horizontal one; the _ext copy is the same
     * value when both are filled. */
    out.horizontalAccuracyMeters = (in->horizontalAccuracyMeters > 0.0f) ? in->horizontalAccuracyMeters : l.accuracy;
    out.verticalAccuracyMeters = in->verticalAccuracyMeters;
    out.speedAccuracyMetersPerSecond = in->speedAccuracyMetersPerSecond;
    out.bearingAccuracyDegrees = in->bearingAccuracyDegrees;
    out.timestampMillis = l.timestamp;
    /* ElapsedRealtime flags have the same bits in both worlds. */
    out.elapsedRealtime.flags = in->elapsedRealtime.flags;
    out.elapsedRealtime.timestampNs = static_cast<int64_t>(in->elapsedRealtime.timestampNs);
    out.elapsedRealtime.timeUncertaintyNs = static_cast<double>(in->elapsedRealtime.timeUncertaintyNs);
    (void)cb->gnssLocationCb(out);
}

void StatusCb(GpsStatus *status)
{
    auto cb = GnssCb();
    if (status == nullptr || cb == nullptr) {
        return;
    }
    /* GpsStatusValue and IGnssCallback.GnssStatusValue share their values. */
    (void)cb->gnssStatusCb(static_cast<gnss::IGnssCallback::GnssStatusValue>(status->status));
}

void LegacySvStatusCb(void *)
{
    /* The GPS-only GpsSvStatus; MTK reports through gnss_sv_status_cb. */
}

void NmeaCb(GpsUtcTime timestamp, const char *nmea, int length)
{
    auto cb = GnssCb();
    if (nmea == nullptr || length <= 0 || cb == nullptr) {
        return;
    }
    (void)cb->gnssNmeaCb(timestamp, std::string(nmea, strnlen(nmea, static_cast<size_t>(length))));
}

void SetCapabilitiesCb(uint32_t capabilities)
{
    auto cb = GnssCb();
    if (cb == nullptr) {
        return;
    }
    /* GPS_CAPABILITY_* and IGnssCallback.CAPABILITY_* agree bit for bit. */
    (void)cb->gnssSetCapabilitiesCb(static_cast<int32_t>(capabilities));
}

void AcquireWakelockCb()
{
}

void ReleaseWakelockCb()
{
}

void RequestUtcTimeCb()
{
    auto cb = GnssCb();
    if (cb != nullptr) {
        (void)cb->gnssRequestTimeCb();
    }
}

void SetSystemInfoCb(const GnssSystemInfo *info)
{
    auto cb = GnssCb();
    if (info == nullptr || cb == nullptr) {
        return;
    }
    gnss::IGnssCallback::GnssSystemInfo out;
    out.yearOfHw = info->year_of_hw;
    out.name = "MediaTek legacy GPS HAL";
    (void)cb->gnssSetSystemInfoCb(out);
}

void GnssSvStatusCb(GnssSvStatus_ext *status)
{
    auto cb = GnssCb();
    if (status == nullptr || cb == nullptr) {
        return;
    }
    int count = status->num_svs;
    if (count < 0) {
        count = 0;
    }
    if (count > MTK_MAX_SV_COUNT) {
        count = MTK_MAX_SV_COUNT;
    }
    std::vector<gnss::IGnssCallback::GnssSvInfo> list;
    list.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        const GnssSvInfo_ext &sv = status->gnss_sv_list[i];
        gnss::IGnssCallback::GnssSvInfo out;
        out.svid = sv.legacySvInfo.svid;
        /* GnssConstellationType numbering is the same (GPS 1 ... IRNSS 7). */
        out.constellation = static_cast<gnss::GnssConstellationType>(sv.legacySvInfo.constellation);
        out.cN0Dbhz = sv.legacySvInfo.c_n0_dbhz;
        out.basebandCN0DbHz = static_cast<float>(sv.basebandCN0DbHz);
        out.elevationDegrees = sv.legacySvInfo.elevation;
        out.azimuthDegrees = sv.legacySvInfo.azimuth;
        out.carrierFrequencyHz = static_cast<int64_t>(sv.carrier_frequency);
        /* GnssSvFlags too (ephemeris 1, almanac 2, used-in-fix 4, carrier 8). */
        out.svFlag = sv.legacySvInfo.flags;
        list.push_back(out);
    }
    (void)cb->gnssSvStatusCb(list);
}

void SetNameCb(const char *name, int length)
{
    if (name != nullptr && length > 0) {
        HG_LOGI("legacy HAL name: %{public}s", std::string(name, strnlen(name, static_cast<size_t>(length))).c_str());
    }
}

void RequestLocationCb(bool independentFromGnss, bool isUserEmergency)
{
    auto cb = GnssCb();
    if (cb != nullptr) {
        (void)cb->gnssRequestLocationCb(independentFromGnss, isUserEmergency);
    }
}

GpsCallbacks_ext g_gpsCallbacks = {
    .size = sizeof(GpsCallbacks_ext),
    .location_cb = LocationCb,
    .status_cb = StatusCb,
    .sv_status_cb = LegacySvStatusCb,
    .nmea_cb = NmeaCb,
    .set_capabilities_cb = SetCapabilitiesCb,
    .acquire_wakelock_cb = AcquireWakelockCb,
    .release_wakelock_cb = ReleaseWakelockCb,
    .create_thread_cb = CreateThread,
    .request_utc_time_cb = RequestUtcTimeCb,
    .set_system_info_cb = SetSystemInfoCb,
    .gnss_sv_status_cb = GnssSvStatusCb,
    .set_name_cb = SetNameCb,
    .request_location_cb = RequestLocationCb,
};

void AGpsStatusCb(AGpsStatusLegacy *status)
{
    auto cb = AGnssCb();
    if (status == nullptr || cb == nullptr) {
        return;
    }
    /* AGpsType / AGpsStatusValue match IAGnssCallback's enums. */
    (void)cb->agnssStatusCb(static_cast<gnss::IAGnssCallback::AGnssType>(status->type),
                            static_cast<gnss::IAGnssCallback::AGnssStatusValue>(status->status));
}

AGpsCallbacks g_agpsCallbacks = {
    .status_cb = AGpsStatusCb,
    .create_thread_cb = CreateThread,
};

void RilRequestSetIdCb(uint32_t flags)
{
    auto cb = RilCb();
    if (cb != nullptr) {
        (void)cb->requestSetIdCb(static_cast<int32_t>(flags));
    }
}

void RilRequestRefLocCb(uint32_t)
{
    auto cb = RilCb();
    if (cb != nullptr) {
        (void)cb->requestRefLocCb();
    }
}

AGpsRilCallbacks g_rilCallbacks = {
    .request_setid = RilRequestSetIdCb,
    .request_refloc = RilRequestRefLocCb,
    .create_thread_cb = CreateThread,
};

/* ── AIDL → HAL ── */

class LegacyAGnss : public gnss::IAGnssDefault {
public:
    explicit LegacyAGnss(const AGpsInterface_ext *agps) : agps_(agps) {}

    ScopedAStatus setCallback(const std::shared_ptr<gnss::IAGnssCallback> &callback) override
    {
        {
            std::lock_guard<std::mutex> guard(State().lock);
            State().agnssCb = callback;
        }
        std::call_once(initOnce_, [this]() { agps_->init(&g_agpsCallbacks); });
        return ScopedAStatus::ok();
    }

    ScopedAStatus dataConnClosed() override
    {
        return Result(agps_->data_conn_closed());
    }

    ScopedAStatus dataConnFailed() override
    {
        return Result(agps_->data_conn_failed());
    }

    ScopedAStatus setServer(gnss::IAGnssCallback::AGnssType type, const std::string &hostname,
                            int32_t port) override
    {
        return Result(agps_->set_server(static_cast<uint16_t>(type), hostname.c_str(), port));
    }

    ScopedAStatus dataConnOpen(int64_t networkHandle, const std::string &apn,
                               gnss::IAGnss::ApnIpType apnIpType) override
    {
        if (agps_->data_conn_open_with_apn_ip_type != nullptr) {
            return Result(agps_->data_conn_open_with_apn_ip_type(static_cast<uint64_t>(networkHandle),
                                                                 apn.c_str(), static_cast<uint16_t>(apnIpType)));
        }
        return Result(agps_->data_conn_open(apn.c_str()));
    }

private:
    static ScopedAStatus Result(int ret)
    {
        /* The legacy calls return 0 on success. */
        return (ret == 0) ? ScopedAStatus::ok() : ScopedAStatus::fromServiceSpecificError(ret);
    }

    const AGpsInterface_ext *agps_;
    std::once_flag initOnce_;
};

class LegacyAGnssRil : public gnss::IAGnssRilDefault {
public:
    explicit LegacyAGnssRil(const AGpsRilInterface_ext *ril) : ril_(ril) {}

    ScopedAStatus setCallback(const std::shared_ptr<gnss::IAGnssRilCallback> &callback) override
    {
        {
            std::lock_guard<std::mutex> guard(State().lock);
            State().rilCb = callback;
        }
        std::call_once(initOnce_, [this]() { ril_->init(&g_rilCallbacks); });
        return ScopedAStatus::ok();
    }

    ScopedAStatus setRefLocation(const gnss::IAGnssRil::AGnssRefLocation &in) override
    {
        AGpsRefLocation ref {};
        /* AGnssRefLocationType: GSM 1, UMTS 2, LTE 4, NR 8 — as in gps.h. */
        ref.type = static_cast<uint16_t>(in.type);
        ref.u.cellID.type = static_cast<uint16_t>(in.cellID.type);
        ref.u.cellID.mcc = static_cast<uint16_t>(in.cellID.mcc);
        ref.u.cellID.mnc = static_cast<uint16_t>(in.cellID.mnc);
        ref.u.cellID.lac = static_cast<uint16_t>(in.cellID.lac);
        ref.u.cellID.cid = static_cast<uint32_t>(in.cellID.cid);
        ref.u.cellID.tac = static_cast<uint16_t>(in.cellID.tac);
        ref.u.cellID.pcid = static_cast<uint16_t>(in.cellID.pcid);
        ril_->set_ref_location(&ref, sizeof(ref));
        return ScopedAStatus::ok();
    }

    ScopedAStatus setSetId(gnss::IAGnssRil::SetIdType type, const std::string &setid) override
    {
        ril_->set_set_id(static_cast<uint16_t>(type), setid.c_str());
        return ScopedAStatus::ok();
    }

    ScopedAStatus updateNetworkState(const gnss::IAGnssRil::NetworkAttributes &attrs) override
    {
        if (ril_->update_network_state_ext != nullptr) {
            ril_->update_network_state_ext(static_cast<uint64_t>(attrs.networkHandle), attrs.isConnected,
                                           static_cast<uint16_t>(attrs.capabilities), attrs.apn.c_str());
        }
        return ScopedAStatus::ok();
    }

private:
    const AGpsRilInterface_ext *ril_;
    std::once_flag initOnce_;
};

class LegacyGnss : public gnss::IGnssDefault {
public:
    explicit LegacyGnss(const GpsInterface_ext *gps) : gps_(gps)
    {
        auto *agps = static_cast<const AGpsInterface_ext *>(gps_->get_extension(HYBRIS_AGPS_INTERFACE));
        if (agps != nullptr) {
            agnss_ = ::ndk::SharedRefBase::make<LegacyAGnss>(agps);
        }
        auto *ril = static_cast<const AGpsRilInterface_ext *>(gps_->get_extension(HYBRIS_AGPS_RIL_INTERFACE));
        if (ril != nullptr) {
            agnssRil_ = ::ndk::SharedRefBase::make<LegacyAGnssRil>(ril);
        }
    }

    ScopedAStatus setCallback(const std::shared_ptr<gnss::IGnssCallback> &callback) override
    {
        {
            std::lock_guard<std::mutex> guard(State().lock);
            State().gnssCb = callback;
        }
        /* GnssBridge re-claims the callback at every session start; the HAL
         * itself is initialised exactly once. */
        int ret = 0;
        std::call_once(initOnce_, [this, &ret]() {
            ret = gps_->init(&g_gpsCallbacks);
            HG_LOGI("legacy GpsInterface.init: %{public}d", ret);
        });
        return (ret == 0) ? ScopedAStatus::ok() : ScopedAStatus::fromServiceSpecificError(ret);
    }

    ScopedAStatus close() override
    {
        gps_->cleanup();
        return ScopedAStatus::ok();
    }

    ScopedAStatus start() override
    {
        return Result("start", gps_->start());
    }

    ScopedAStatus stop() override
    {
        return Result("stop", gps_->stop());
    }

    /* The legacy HAL reports SV status and NMEA for every session. */
    ScopedAStatus startSvStatus() override
    {
        return ScopedAStatus::ok();
    }

    ScopedAStatus stopSvStatus() override
    {
        return ScopedAStatus::ok();
    }

    ScopedAStatus startNmea() override
    {
        return ScopedAStatus::ok();
    }

    ScopedAStatus stopNmea() override
    {
        return ScopedAStatus::ok();
    }

    ScopedAStatus injectTime(int64_t timeMs, int64_t timeReferenceMs, int32_t uncertaintyMs) override
    {
        return Result("inject_time", gps_->inject_time(timeMs, timeReferenceMs, uncertaintyMs));
    }

    ScopedAStatus injectLocation(const gnss::GnssLocation &location) override
    {
        return Result("inject_location",
                      gps_->inject_location(location.latitudeDegrees, location.longitudeDegrees,
                                            location.horizontalAccuracyMeters));
    }

    ScopedAStatus injectBestLocation(const gnss::GnssLocation &location) override
    {
        if (gps_->inject_fused_location != nullptr) {
            return Result("inject_fused_location",
                          gps_->inject_fused_location(location.latitudeDegrees, location.longitudeDegrees,
                                                      location.horizontalAccuracyMeters));
        }
        return injectLocation(location);
    }

    ScopedAStatus deleteAidingData(gnss::IGnss::GnssAidingData flags) override
    {
        /* IGnss.GnssAidingData is the legacy GpsAidingData, bit for bit. */
        gps_->delete_aiding_data(static_cast<GpsAidingData>(static_cast<int32_t>(flags) & 0xFFFF));
        return ScopedAStatus::ok();
    }

    ScopedAStatus setPositionMode(const gnss::IGnss::PositionModeOptions &options) override
    {
        return Result("set_position_mode",
                      gps_->set_position_mode(static_cast<GpsPositionMode>(options.mode),
                                              static_cast<GpsPositionRecurrence>(options.recurrence),
                                              static_cast<uint32_t>(options.minIntervalMs),
                                              static_cast<uint32_t>(options.preferredAccuracyMeters),
                                              static_cast<uint32_t>(options.preferredTimeMs),
                                              options.lowPowerMode));
    }

    ScopedAStatus getExtensionAGnss(std::shared_ptr<gnss::IAGnss> *ret) override
    {
        *ret = agnss_;
        return ScopedAStatus::ok();
    }

    ScopedAStatus getExtensionAGnssRil(std::shared_ptr<gnss::IAGnssRil> *ret) override
    {
        *ret = agnssRil_;
        return ScopedAStatus::ok();
    }

    ScopedAStatus getInterfaceVersion(int32_t *ret) override
    {
        /* Not an AIDL HAL; 0 tags the legacy backend in the bridge's log. */
        *ret = 0;
        return ScopedAStatus::ok();
    }

private:
    static ScopedAStatus Result(const char *what, int ret)
    {
        if (ret != 0) {
            HG_LOGW("legacy %{public}s: %{public}d", what, ret);
            return ScopedAStatus::fromServiceSpecificError(ret);
        }
        return ScopedAStatus::ok();
    }

    const GpsInterface_ext *gps_;
    std::shared_ptr<gnss::IAGnss> agnss_;
    std::shared_ptr<gnss::IAGnssRil> agnssRil_;
    std::once_flag initOnce_;
};

} // namespace

std::shared_ptr<gnss::IGnss> OpenLegacyGnss()
{
    const GpsInterface_ext *gps = OpenLegacyHal();
    if (gps == nullptr) {
        return nullptr;
    }
    return ::ndk::SharedRefBase::make<LegacyGnss>(gps);
}

} // namespace HybrisGnss
} // namespace OHOS
