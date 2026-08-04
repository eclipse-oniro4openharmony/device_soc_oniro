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
 *
 * WLAN "extend" VDI for hybris_generic. wlan_interface_service refuses to
 * come up at all when HdfLoadVdi(libhdi_wlan_impl.z.so) fails, which took
 * the whole base WLAN HDI (nl80211 over the Halium driver, working) down
 * with it. The extend surface itself is Huawei-vendor territory — channel
 * measurement, hid2d, private ioctls — that the MediaTek/Halium stack does
 * not expose, so every operation honestly reports NOT_SUPPORT; only the
 * construct/destruct lifecycle succeeds.
 */

#include <hdf_base.h>
#include <hdf_log.h>
#include "wlan_extend_cmd_vdi.h"

#define HDF_LOG_TAG hybris_wlan_extend_vdi

static int32_t StartChannelMeasVdi(struct IWlanInterface *self, const char *ifName,
    const struct MeasChannelParam *measChannelParam)
{
    (void)self;
    (void)ifName;
    (void)measChannelParam;
    return HDF_ERR_NOT_SUPPORT;
}

static int32_t GetChannelMeasResultVdi(struct IWlanInterface *self, const char *ifName,
    struct MeasChannelResult *measChannelResult)
{
    (void)self;
    (void)ifName;
    (void)measChannelResult;
    return HDF_ERR_NOT_SUPPORT;
}

static int32_t SendCmdIoctlVdi(struct IWlanInterface *self, const char *ifName, int32_t cmdId,
    const int8_t *paramBuf, uint32_t paramBufLen)
{
    (void)self;
    (void)ifName;
    (void)cmdId;
    (void)paramBuf;
    (void)paramBufLen;
    return HDF_ERR_NOT_SUPPORT;
}

static int32_t GetCoexChannelListVdi(struct IWlanInterface *self, const char *ifName,
    uint8_t *paramBuf, uint32_t *paramBufLen)
{
    (void)self;
    (void)ifName;
    (void)paramBuf;
    (void)paramBufLen;
    return HDF_ERR_NOT_SUPPORT;
}

static int32_t RegisterHid2dCallbackVdi(Hid2dCallbackFunc func, const char *ifName)
{
    (void)func;
    (void)ifName;
    return HDF_ERR_NOT_SUPPORT;
}

static int32_t UnregisterHid2dCallbackVdi(Hid2dCallbackFunc func, const char *ifName)
{
    (void)func;
    (void)ifName;
    return HDF_ERR_NOT_SUPPORT;
}

static int32_t WifiConstructVdi(void)
{
    HDF_LOGI("%{public}s: hybris wlan extend vdi ready (all extend ops NOT_SUPPORT)", __func__);
    return HDF_SUCCESS;
}

static int32_t WifiDestructVdi(void)
{
    return HDF_SUCCESS;
}

static struct WlanExtendInterfaceVdi g_wlanExtendModule = {
    .startChannelMeas = StartChannelMeasVdi,
    .getChannelMeasResult = GetChannelMeasResultVdi,
    .sendCmdIoctl = SendCmdIoctlVdi,
    .getCoexChannelList = GetCoexChannelListVdi,
    .registerHid2dCallback = RegisterHid2dCallbackVdi,
    .unregisterHid2dCallback = UnregisterHid2dCallbackVdi,
    .wifiConstruct = WifiConstructVdi,
    .wifiDestruct = WifiDestructVdi,
};

static struct VdiWrapperWlanExtend g_hybrisWlanExtendVdi = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "wlan_service",
    },
    .wlanExtendModule = &g_wlanExtendModule,
};

HDF_VDI_INIT(g_hybrisWlanExtendVdi);
