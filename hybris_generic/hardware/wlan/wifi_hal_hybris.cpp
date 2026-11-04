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
 *
 * It also replaces getScanResults, to stop a recurring wifi_manager_service
 * crash (SIGSEGV in WifiStaHalInterface::QueryScanInfos → GetScanResultText
 * → HdiBssGetVendorBeacon).  Two upstream bugs stack: wifi_scan.cpp fills
 * ScanResultsInfo.beaconIe from NL80211_BSS_INFORMATION_ELEMENTS (a second
 * copy of .ie, not NL80211_BSS_BEACON_IES), and the framework then walks
 * beaconIe.size() bytes starting at ie.data() + ie.size() — past the end of
 * the .ie vector, as if the two were packed back to back like
 * wpa_supplicant's bss buffer.  Whether that faults depends on what follows
 * the heap block, hence the intermittent crashes.  beaconIe has no other
 * reader and holds nothing .ie does not, so it is simply emptied, which
 * turns the out-of-bounds walk into its beaconIeLen == 0 early return.
 */

#include <cstring>
#include <vector>
#include <unistd.h>
#include <hdf_log.h>
#include "parameter.h"
#include <cerrno>
#include <cstdio>
#include <string>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/sockios.h>
#include "wifi_hal.h"
#include "common.h" /* GetIfaceInfo; needs IFNAMSIZ from <net/if.h> first */

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

using GetScanResultsFn = WifiError (*)(wifiInterfaceHandle,
    std::vector<OHOS::HDI::Wlan::Chip::V2_0::ScanResultsInfo>&);
GetScanResultsFn g_upstreamGetScanResults = nullptr;

WifiError HybrisGetScanResults(wifiInterfaceHandle handle,
    std::vector<OHOS::HDI::Wlan::Chip::V2_0::ScanResultsInfo>& results)
{
    WifiError ret = g_upstreamGetScanResults(handle, results);
    for (auto &result : results) {
        result.beaconIe.clear();
    }
    return ret;
}

/*
 * Country code.  Upstream sends it as the Google-OUI vendor subcommand
 * WIFI_SUBCMD_SET_COUNTRY_CODE, which gen4m rejects (-1).  WifiApStartedState
 * treats that failure as fatal, so the hotspot came up and was torn down
 * before hostapd ever ran ("set countryCode=CN failed").
 *
 * gen4m takes the Android driver command "COUNTRY <cc>" on SIOCDEVPRIVATE+1
 * (priv_support_driver_cmd), the path MTK's own wifi HAL uses.  Its argument
 * is struct priv_driver_cmd_s with the buffer INLINE — which is also why the
 * upstream pointer-layout WifiPrivCmd got its pointer overwritten by the
 * "UNSUPPORTED" reply (see wifi_ioctl.cpp).
 */
constexpr size_t MTK_PRIV_CMD_SIZE = 512;
struct MtkPrivDriverCmd {
    char buf[MTK_PRIV_CMD_SIZE];
    int usedLen;
    int totalLen;
};

using SetCountryCodeFn = WifiError (*)(wifiInterfaceHandle, const char *);
SetCountryCodeFn g_upstreamSetCountryCode = nullptr;

WifiError MtkDriverCmd(const char *ifName, const std::string &cmd)
{
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        return HAL_UNKNOWN;
    }
    MtkPrivDriverCmd priv = {};
    (void)snprintf(priv.buf, sizeof(priv.buf), "%s", cmd.c_str());
    priv.usedLen = static_cast<int>(strlen(priv.buf)) + 1;
    priv.totalLen = static_cast<int>(sizeof(priv.buf));
    struct ifreq ifr = {};
    (void)snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifName);
    ifr.ifr_data = reinterpret_cast<char *>(&priv);
    int ret = ioctl(sock, SIOCDEVPRIVATE + 1, &ifr);
    int err = errno;
    close(sock);
    if (ret < 0) {
        HDF_LOGE("%{public}s on %{public}s failed: %{public}d", cmd.c_str(), ifName, err);
        return HAL_UNKNOWN;
    }
    priv.buf[sizeof(priv.buf) - 1] = '\0';
    if (strncmp(priv.buf, "UNSUPPORTED", strlen("UNSUPPORTED")) == 0) {
        HDF_LOGE("%{public}s on %{public}s: driver says UNSUPPORTED", cmd.c_str(), ifName);
        return HAL_NOT_SUPPORTED;
    }
    return HAL_SUCCESS;
}

WifiError HybrisSetCountryCode(wifiInterfaceHandle handle, const char *countryCode)
{
    if (countryCode == nullptr) {
        return HAL_INVALID_ARGS;
    }
    if (g_upstreamSetCountryCode != nullptr && g_upstreamSetCountryCode(handle, countryCode) == HAL_SUCCESS) {
        return HAL_SUCCESS;
    }
    InterfaceInfo *info = GetIfaceInfo(handle);
    const char *ifName = (info != nullptr && info->name[0] != '\0') ? info->name : "wlan0";
    WifiError ret = MtkDriverCmd(ifName, std::string("COUNTRY ") + countryCode);
    HDF_LOGI("set country code %{public}s on %{public}s via driver command: %{public}d", countryCode,
        ifName, ret);
    return ret;
}

/*
 * STA/AP coexistence.  gen4m cannot turn wlan0 into an AP
 * (NL80211_CMD_SET_INTERFACE to AP fails with -EFAULT); like Android's
 * ro.vendor.wifi.sap.interface=ap0 it wants a separate AP interface.  With
 * coex reported, the chip HDI names the AP "wlan1" and creates it with
 * NL80211_CMD_NEW_INTERFACE (type AP), and uses the STA+AP+P2P mode table,
 * so the STA link also stays up while the hotspot runs.
 */
WifiError HybrisIsSupportCoex(bool &isCoex)
{
    isCoex = true;
    return HAL_SUCCESS;
}
} // namespace

extern "C" WifiError UpstreamInitWifiVendorHalFuncTable(WifiHalFn *fn);

extern "C" WifiError InitWifiVendorHalFuncTable(WifiHalFn *fn)
{
    WifiError ret = UpstreamInitWifiVendorHalFuncTable(fn);
    if (ret == HAL_SUCCESS) {
        fn->waitDriverStart = HybrisWaitDriverStart;
        if (fn->getScanResults != nullptr) {
            g_upstreamGetScanResults = fn->getScanResults;
            fn->getScanResults = HybrisGetScanResults;
        }
        g_upstreamSetCountryCode = fn->wifiSetCountryCode;
        fn->wifiSetCountryCode = HybrisSetCountryCode;
        fn->isSupportCoex = HybrisIsSupportCoex;
    }
    return ret;
}
