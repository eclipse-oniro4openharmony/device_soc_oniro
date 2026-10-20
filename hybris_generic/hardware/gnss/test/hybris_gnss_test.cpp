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
 * Drives vendorGnssAdapter.so through exactly the ABI location_host uses —
 * dlopen by name, dlsym `GnssVendorInterface`, enable → config → start —
 * and prints what comes back, so the HAL seam can be proven without the
 * location framework in the loop.
 *
 *     hybris_gnss_test [seconds] [path-to-vendorGnssAdapter.so] [mode]
 *
 * mode is GnssWorkingMode: 1 standalone (default), 2 MS-based, 3 MS-assisted.
 *
 * Stop location_host first (or run while the location switch is off): the
 * HAL takes one callback, and the last setCallback wins.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#include <dlfcn.h>
#include <unistd.h>

#include "location_vendor_lib.h"

using namespace OHOS::HDI::Location;

namespace {

std::atomic<int> g_fixes{0};
std::atomic<int> g_svReports{0};
std::atomic<int> g_nmea{0};

double Now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double g_t0 = 0;

void OnLocation(GnssLocation *loc)
{
    g_fixes++;
    printf("[%7.1f] FIX lat %.6f lon %.6f alt %.1f acc %.1f m speed %.1f valid 0x%x\n",
           Now() - g_t0, loc->latitude, loc->longitude, loc->altitude, loc->horizontalAccuracy,
           loc->speed, loc->fieldValidity);
    fflush(stdout);
}

void OnStatus(uint16_t *status)
{
    printf("[%7.1f] STATUS %u\n", Now() - g_t0, *status);
    fflush(stdout);
}

void OnSv(GnssSatelliteStatus *sv)
{
    if (g_svReports++ % 5 != 0) {
        return;
    }
    unsigned used = 0;
    float best = 0;
    for (uint32_t i = 0; i < sv->satellitesNum; i++) {
        if (sv->satellitesList[i].satelliteAdditionalInfo &
            static_cast<uint32_t>(SatelliteAdditionalInfo::SATELLITES_ADDITIONAL_INFO_USED_IN_FIX)) {
            used++;
        }
        if (sv->satellitesList[i].cn0 > best) {
            best = sv->satellitesList[i].cn0;
        }
    }
    printf("[%7.1f] SV %u visible, %u used, best C/N0 %.1f dB-Hz\n", Now() - g_t0,
           sv->satellitesNum, used, best);
    fflush(stdout);
}

void OnNmea(int64_t time, const char *nmea, int length)
{
    if (g_nmea++ == 0) {
        printf("[%7.1f] first NMEA: %.*s\n", Now() - g_t0, length, nmea);
        fflush(stdout);
    }
}

void OnCapabilities(uint32_t caps)
{
    printf("[%7.1f] CAPABILITIES 0x%x\n", Now() - g_t0, caps);
    fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 60;
    const char *path = (argc > 2) ? argv[2] : "vendorGnssAdapter.so";
    int mode = (argc > 3) ? atoi(argv[3]) : static_cast<int>(GnssWorkingMode::GNSS_WORKING_MODE_STANDALONE);
    g_t0 = Now();

    void *lib = dlopen(path, RTLD_NOW);
    if (lib == nullptr) {
        printf("!! dlopen %s: %s\n", path, dlerror());
        return 1;
    }
    auto *device = static_cast<GnssVendorDevice *>(dlsym(lib, "GnssVendorInterface"));
    if (device == nullptr || device->getGnssInterface == nullptr) {
        printf("!! no GnssVendorInterface in %s\n", path);
        return 1;
    }
    const GnssVendorInterface *gnss = device->getGnssInterface();

    static GnssCallbackStruct callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.size = sizeof(callbacks);
    callbacks.gnssCallback.size = sizeof(GnssCallbackStruct);
    callbacks.gnssCallback.locationUpdate = OnLocation;
    callbacks.gnssCallback.gnssWorkingStatusUpdate = OnStatus;
    callbacks.gnssCallback.satelliteStatusUpdate = OnSv;
    callbacks.gnssCallback.nmeaUpdate = OnNmea;
    callbacks.gnssCallback.capabilitiesUpdate = OnCapabilities;

    int ret = gnss->enableGnss(&callbacks);
    printf("[%7.1f] enableGnss -> %d\n", Now() - g_t0, ret);
    if (ret != 0) {
        return 1;
    }

    GnssConfigParameter para;
    memset(&para, 0, sizeof(para));
    para.size = sizeof(para);
    para.startCategory = static_cast<uint32_t>(GnssStartCategory::GNSS_START_CATEGORY_NORMAL);
    para.u.gnssBasicConfig.size = sizeof(GnssBasicConfigPara);
    para.u.gnssBasicConfig.minInterval = 1000;
    para.u.gnssBasicConfig.gnssMode = mode;
    printf("[%7.1f] setGnssConfigPara -> %d\n", Now() - g_t0, gnss->setGnssConfigPara(&para));

    /* Seed the clock the way the framework's NTP path does. */
    GnssReferenceInfo ref;
    memset(&ref, 0, sizeof(ref));
    ref.size = sizeof(ref);
    ref.category = static_cast<int>(GnssRefInfoCategory::GNSS_REF_INFO_CATEGORY_TIME);
    struct timespec rt;
    struct timespec bt;
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_BOOTTIME, &bt);
    ref.u.time.size = sizeof(GnssRefTime);
    ref.u.time.time = rt.tv_sec * 1000LL + rt.tv_nsec / 1000000;
    ref.u.time.elapsedRealtime = bt.tv_sec * 1000LL + bt.tv_nsec / 1000000;
    ref.u.time.uncertaintyOfTime = 1000;
    printf("[%7.1f] inject time -> %d\n", Now() - g_t0,
           gnss->injectsGnssReferenceInfo(ref.category, &ref));

    ret = gnss->startGnss(static_cast<uint32_t>(GnssStartCategory::GNSS_START_CATEGORY_NORMAL));
    printf("[%7.1f] startGnss -> %d; listening %d s\n", Now() - g_t0, ret, seconds);

    std::this_thread::sleep_for(std::chrono::seconds(seconds));

    gnss->stopGnss(static_cast<uint32_t>(GnssStartCategory::GNSS_START_CATEGORY_NORMAL));
    gnss->disableGnss();
    printf("[%7.1f] done: %d fixes, %d SV reports, %d NMEA sentences\n", Now() - g_t0,
           g_fixes.load(), g_svReports.load(), g_nmea.load());
    fflush(stdout);
    /* _exit: the container's binder threads are still running in our
     * address space, and static destructors would race them. */
    _exit(0);
}
