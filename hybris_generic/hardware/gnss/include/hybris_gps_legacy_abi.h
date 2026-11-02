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

#ifndef HYBRIS_GPS_LEGACY_ABI_H
#define HYBRIS_GPS_LEGACY_ABI_H

/*
 * The binary interface of MediaTek's legacy GPS HAL module (gps.default.so,
 * the "hal2mnl" client of mnld) as shipped by Halium 12 MTK vendors such as
 * the Volla X23.  Android's own gnss@2.1 / AIDL services are thin wrappers
 * around this module, so loading it in-process gives the full session API
 * that the vendor's Android 12 AIDL IGnss (v1, extensions only) lacks.
 *
 * Only the members this bridge touches are mirrored, at their exact offsets:
 * AOSP <hardware/gps.h> (GpsLocation, GnssSvInfo, GpsStatus, gps_device_t)
 * plus MediaTek's <hardware/gps_mtk.h> extensions of the Android 11/12 era
 * (GpsLocation_ext, GnssSvInfo_ext, GnssSvStatus_ext, GpsCallbacks_ext,
 * GpsInterface_ext, AGpsInterface_ext, AGpsRilInterface_ext).  Every struct
 * that the HAL reads from us is passed whole, with `size` set, so the HAL's
 * own size checks see the layout it was built for.
 */

#include <cstddef>
#include <cstdint>
#include <pthread.h>

extern "C" {

/* <hardware/hardware.h>, LP64 layout. */
struct hw_device_t;
struct hw_module_t;

struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id, struct hw_device_t **device);
};

struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};

struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *device);
};

#define HYBRIS_GPS_HARDWARE_MODULE_ID "gps"

using GpsUtcTime = int64_t;
using GpsPositionMode = uint32_t;
using GpsPositionRecurrence = uint32_t;
using GpsAidingData = uint16_t;
using GpsStatusValue = uint16_t;
using GnssConstellationTypeLegacy = uint8_t;
using GnssSvFlagsLegacy = uint8_t;

constexpr GpsPositionMode GPS_POSITION_MODE_STANDALONE = 0;
constexpr GpsPositionMode GPS_POSITION_MODE_MS_BASED = 1;
constexpr GpsPositionMode GPS_POSITION_MODE_MS_ASSISTED = 2;
constexpr GpsPositionRecurrence GPS_POSITION_RECURRENCE_PERIODIC = 0;

constexpr GpsStatusValue GPS_STATUS_NONE = 0;
constexpr GpsStatusValue GPS_STATUS_SESSION_BEGIN = 1;
constexpr GpsStatusValue GPS_STATUS_SESSION_END = 2;
constexpr GpsStatusValue GPS_STATUS_ENGINE_ON = 3;
constexpr GpsStatusValue GPS_STATUS_ENGINE_OFF = 4;

/* GpsLocation.flags */
constexpr uint16_t GPS_LOCATION_HAS_LAT_LONG = 0x0001;
constexpr uint16_t GPS_LOCATION_HAS_ALTITUDE = 0x0002;
constexpr uint16_t GPS_LOCATION_HAS_SPEED = 0x0004;
constexpr uint16_t GPS_LOCATION_HAS_BEARING = 0x0008;
constexpr uint16_t GPS_LOCATION_HAS_HORIZONTAL_ACCURACY = 0x0010;
/* MediaTek's GpsLocation_ext adds the GnssLocation 1.0 accuracies. */
constexpr uint16_t GPS_LOCATION_HAS_VERTICAL_ACCURACY = 0x0020;
constexpr uint16_t GPS_LOCATION_HAS_SPEED_ACCURACY = 0x0040;
constexpr uint16_t GPS_LOCATION_HAS_BEARING_ACCURACY = 0x0080;

/* GnssSvInfo.flags */
constexpr uint8_t GNSS_SV_FLAGS_HAS_EPHEMERIS_DATA = 1 << 0;
constexpr uint8_t GNSS_SV_FLAGS_HAS_ALMANAC_DATA = 1 << 1;
constexpr uint8_t GNSS_SV_FLAGS_USED_IN_FIX = 1 << 2;
constexpr uint8_t GNSS_SV_FLAGS_HAS_CARRIER_FREQUENCY = 1 << 3;

/* AGpsType / AGpsSetIDType */
constexpr uint16_t AGPS_TYPE_SUPL = 1;
constexpr uint16_t AGPS_SETID_TYPE_NONE = 0;
constexpr uint16_t AGPS_SETID_TYPE_IMSI = 1;
constexpr uint16_t AGPS_SETID_TYPE_MSISDN = 2;

constexpr int MTK_MAX_SV_COUNT = 195;

struct GpsLocation {
    size_t size;
    uint16_t flags;
    double latitude;
    double longitude;
    double altitude;
    float speed;
    float bearing;
    float accuracy;
    GpsUtcTime timestamp;
};

struct ElapsedRealtimeLegacy {
    uint16_t flags;
    uint64_t timestampNs;
    uint64_t timeUncertaintyNs;
};

struct GpsLocation_ext {
    GpsLocation legacyLocation;
    float horizontalAccuracyMeters;
    float verticalAccuracyMeters;
    float speedAccuracyMetersPerSecond;
    float bearingAccuracyDegrees;
    ElapsedRealtimeLegacy elapsedRealtime;
};

struct GpsStatus {
    size_t size;
    GpsStatusValue status;
};

struct GnssSvInfo {
    size_t size;
    int16_t svid;
    GnssConstellationTypeLegacy constellation;
    float c_n0_dbhz;
    float elevation;
    float azimuth;
    GnssSvFlagsLegacy flags;
};

struct GnssSvInfo_ext {
    GnssSvInfo legacySvInfo;
    float carrier_frequency;
    double basebandCN0DbHz;
};

struct GnssSvStatus_ext {
    size_t size;
    int num_svs;
    GnssSvInfo_ext gnss_sv_list[MTK_MAX_SV_COUNT];
};

struct GnssSystemInfo {
    size_t size;
    uint16_t year_of_hw;
};

using gps_location_ext_callback = void (*)(GpsLocation_ext *location);
using gps_status_callback = void (*)(GpsStatus *status);
using gps_sv_status_callback = void (*)(void *svInfo); /* legacy GpsSvStatus, unused */
using gps_nmea_callback = void (*)(GpsUtcTime timestamp, const char *nmea, int length);
using gps_set_capabilities = void (*)(uint32_t capabilities);
using gps_acquire_wakelock = void (*)();
using gps_release_wakelock = void (*)();
using gps_create_thread = pthread_t (*)(const char *name, void (*start)(void *), void *arg);
using gps_request_utc_time = void (*)();
using gnss_set_system_info = void (*)(const GnssSystemInfo *info);
using gnss_sv_status_ext_callback = void (*)(GnssSvStatus_ext *svInfo);
using gnss_set_name_callback = void (*)(const char *name, int length);
using gnss_request_location_callback = void (*)(bool independentFromGnss, bool isUserEmergency);

struct GpsCallbacks_ext {
    size_t size;
    gps_location_ext_callback location_cb;
    gps_status_callback status_cb;
    gps_sv_status_callback sv_status_cb;
    gps_nmea_callback nmea_cb;
    gps_set_capabilities set_capabilities_cb;
    gps_acquire_wakelock acquire_wakelock_cb;
    gps_release_wakelock release_wakelock_cb;
    gps_create_thread create_thread_cb;
    gps_request_utc_time request_utc_time_cb;
    gnss_set_system_info set_system_info_cb;
    gnss_sv_status_ext_callback gnss_sv_status_cb;
    gnss_set_name_callback set_name_cb;
    gnss_request_location_callback request_location_cb;
};

struct GpsInterface_ext {
    size_t size;
    int (*init)(GpsCallbacks_ext *callbacks);
    int (*start)();
    int (*stop)();
    void (*cleanup)();
    int (*inject_time)(GpsUtcTime time, int64_t timeReference, int uncertainty);
    int (*inject_location)(double latitude, double longitude, float accuracy);
    void (*delete_aiding_data)(GpsAidingData flags);
    int (*set_position_mode)(GpsPositionMode mode, GpsPositionRecurrence recurrence,
                             uint32_t minInterval, uint32_t preferredAccuracy,
                             uint32_t preferredTime, bool lowPowerMode);
    const void *(*get_extension)(const char *name);
    int (*inject_fused_location)(double latitude, double longitude, float accuracy);
};

struct gps_device_t {
    struct hw_device_t common;
    const GpsInterface_ext *(*get_gps_interface)(struct gps_device_t *dev);
};

/* ── AGPS ("agps" extension) ── */

struct AGpsStatusLegacy {
    size_t size;
    uint16_t type;
    uint16_t status;
    uint32_t ipv4_addr;
    uint8_t addr[128]; /* struct sockaddr_storage */
};

using agps_status_callback = void (*)(AGpsStatusLegacy *status);

struct AGpsCallbacks {
    agps_status_callback status_cb;
    gps_create_thread create_thread_cb;
};

struct AGpsInterface_ext {
    size_t size;
    void (*init)(AGpsCallbacks *callbacks);
    int (*data_conn_open)(const char *apn);
    int (*data_conn_closed)();
    int (*data_conn_failed)();
    int (*set_server)(uint16_t type, const char *hostname, int port);
    int (*data_conn_open_with_apn_ip_type)(uint64_t networkHandle, const char *apn, uint16_t apnIpType);
};

/* ── AGPS RIL ("agps_ril" extension) ── */

struct AGpsRefLocationCellID {
    uint16_t type;
    uint16_t mcc;
    uint16_t mnc;
    uint16_t lac;
    uint32_t cid;
    uint16_t tac;
    uint16_t pcid;
};

struct AGpsRefLocation {
    uint16_t type;
    union {
        AGpsRefLocationCellID cellID;
        uint8_t mac[8];
    } u;
};

using agps_ril_request_set_id = void (*)(uint32_t flags);
using agps_ril_request_ref_loc = void (*)(uint32_t flags);

struct AGpsRilCallbacks {
    agps_ril_request_set_id request_setid;
    agps_ril_request_ref_loc request_refloc;
    gps_create_thread create_thread_cb;
};

struct AGpsRilInterface_ext {
    size_t size;
    void (*init)(AGpsRilCallbacks *callbacks);
    void (*set_ref_location)(const AGpsRefLocation *refLocation, size_t szStruct);
    void (*set_set_id)(uint16_t type, const char *setid);
    void (*ni_message)(uint8_t *msg, size_t len);
    void (*update_network_state_ext)(uint64_t networkHandle, bool isConnected,
                                     uint16_t capabilities, const char *apn);
    void (*update_network_availability)(int available, const char *apn);
};

#define HYBRIS_AGPS_INTERFACE "agps"
#define HYBRIS_AGPS_RIL_INTERFACE "agps_ril"

} // extern "C"

#endif // HYBRIS_GPS_LEGACY_ABI_H
