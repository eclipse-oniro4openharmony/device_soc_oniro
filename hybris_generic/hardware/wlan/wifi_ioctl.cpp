/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.
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
 * hybris_generic override of upstream drivers/peripheral/wlan/chip/wifi_hal/wifi_ioctl.cpp
 *
 * The upstream implementation of GetChipCaps() and WifiGetSupportedFeatureSet() uses
 * ioctl(SIOCDEVPRIVATE + 1), a Huawei/HiSilicon vendor-specific ioctl. On MediaTek
 * hardware (gen4m-6789), this ioctl returns the ASCII string "UNSUPPORTED" in the
 * response buffer instead of a proper integer. The upstream code then does:
 *     memcpy_s(cmd, MAX_CMD_LEN, privCmd.buf, MAX_CMD_LEN - 1)
 * where privCmd.buf has been corrupted to point at the "UNSUPPORTED" string data
 * (0x524f505055534e55 = "UNSUPPOR" reversed), causing SIGSEGV in memcpy.
 *
 * This override stubs out the vendor-specific ioctl functions while keeping all
 * other functions (GetPowerMode, SetPowerMode, etc.) identical to upstream.
 */

#include <string>
#include "wifi_hal.h"
#include "wifi_ioctl.h"
#include <securec.h>
#include <osal_mem.h>
#include <hdf_log.h>

WifiError GetPowerMode(const char *ifName, int *mode)
{
    return HAL_SUCCESS;
}

WifiError SetPowerMode(const char *ifName, int mode)
{
    return HAL_SUCCESS;
}

WifiError SetTxPower(const char *ifName, int mode)
{
    return HAL_SUCCESS;
}

WifiError EnablePowerMode(const char *ifName, int mode)
{
    return HAL_SUCCESS;
}

uint32_t WifiGetSupportedFeatureSet(const char *ifName)
{
    // Vendor-specific ioctl (SIOCDEVPRIVATE+1) not supported on MediaTek.
    // Return 0 = no special features.
    (void)ifName;
    HDF_LOGI("WifiGetSupportedFeatureSet: returning 0 (vendor ioctl unsupported on hybris_generic)");
    return 0;
}

uint32_t GetChipCaps(const char *ifName)
{
    // Vendor-specific ioctl (SIOCDEVPRIVATE+1) not supported on MediaTek.
    // Return 0 = no special caps.
    (void)ifName;
    HDF_LOGI("GetChipCaps: returning 0 (vendor ioctl unsupported on hybris_generic)");
    return 0;
}
