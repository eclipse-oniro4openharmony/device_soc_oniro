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
 * hybris_generic wrapper around upstream's InitWifiVendorHalFuncTable().
 *
 * Upstream wifi_hal.cpp is compiled with InitWifiVendorHalFuncTable renamed
 * (see BUILD.gn); this file provides the real exported entry point, fills
 * the table through upstream and then replaces waitDriverStart.
 *
 * Why: upstream WaitDriverStart() treats /sys/class/net/wlan0 existing as
 * "driver ready".  On MediaTek connac2 (Plinius / mt6878) the netdev is
 * registered by a 'P' probe write well before the WiFi function is armed,
 * and wmtdetect-init's arming "1" write then blocks ~3.5 s in the kernel.
 * The framework's one-shot STA auto-start lands inside that window on some
 * boots, wpa_supplicant fails "Could not set interface wlan0 flags (UP): No
 * such device", and the phone boots with WiFi off.
 *
 * The device's init cfg sets ohos.hybris.wifi_armed=0 before it starts the
 * wmtwifi-on helper, and the helper sets it to 1 (or "failed") when done.
 * While it reads 0 we hold the vendor HAL start, which is the first step of
 * every STA start (CreateStaIface -> chip configuration -> Start()) and
 * precedes wpa_supplicant.  A device that never sets the param (the X23,
 * where wlan0 only appears once armed) keeps upstream behaviour unchanged.
 */

#include <cstring>
#include <unistd.h>
#include <hdf_log.h>
#include "parameter.h"
#include "wifi_hal.h"

#undef InitWifiVendorHalFuncTable

namespace {
constexpr const char *WIFI_ARMED_PARAM = "ohos.hybris.wifi_armed";
constexpr int ARMED_POLL_US = 100000;
// Arming normally completes within a few seconds of the framework asking;
// the cap only bounds a wedged helper, after which upstream's check runs.
constexpr int ARMED_WAIT_MAX_MS = 30000;

bool ArmingPending()
{
    char value[16] = {0};
    return GetParameter(WIFI_ARMED_PARAM, "", value, sizeof(value)) > 0 && strcmp(value, "0") == 0;
}

WifiError HybrisWaitDriverStart(void)
{
    if (ArmingPending()) {
        HDF_LOGI("waiting for the WiFi function to be armed (%{public}s)", WIFI_ARMED_PARAM);
        int count = (ARMED_WAIT_MAX_MS * 1000) / ARMED_POLL_US;
        while (ArmingPending() && --count > 0) {
            usleep(ARMED_POLL_US);
        }
        if (ArmingPending()) {
            HDF_LOGE("timed out waiting for %{public}s, starting anyway", WIFI_ARMED_PARAM);
        } else {
            HDF_LOGI("WiFi function armed");
        }
    }
    return WaitDriverStart();
}
} // namespace

extern "C" WifiError UpstreamInitWifiVendorHalFuncTable(WifiHalFn *fn);

extern "C" WifiError InitWifiVendorHalFuncTable(WifiHalFn *fn)
{
    WifiError ret = UpstreamInitWifiVendorHalFuncTable(fn);
    if (ret == HAL_SUCCESS) {
        fn->waitDriverStart = HybrisWaitDriverStart;
    }
    return ret;
}
