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
 * libril_vendor_hybris — the OHOS vendor RIL for hybris_generic.
 *
 * hril_hdf.c dlopens the library named by the system parameter
 * `const.sys.radio.vendorlib.path` and resolves exactly one symbol,
 * RilInitOps().  That is the whole contract: no HDF driver, no HDI service,
 * no framework patch.  We ship under a name of our own so we never collide
 * with upstream's AT-modem sample (libril_vendor.z.so), and init.ansuz.cfg
 * points the parameter at us — on any other device the parameter stays
 * unset, LoadVendor() logs "no vendor lib" and riladapter_host idles, which
 * is exactly what those devices do today.
 *
 * Ops tables may be partial: hril answers a NULL member with
 * RIL_ERR_VENDOR_NOT_IMPLEMENT, so the domains land one phase at a time
 * (plan §D4).  Currently: modem + the SIM floor.
 */

#include "hril.h"
#include "hybris_ril_bridge.h"
#include "hybris_ril_log.h"

using namespace OHOS::HybrisRil;

static HRilOps g_hrilOps = {
    .version = 1,
    .callOps = nullptr,     /* R6 */
    .simOps = nullptr,      /* filled in below */
    .smsOps = nullptr,      /* R4 */
    .dataOps = nullptr,     /* R5 */
    .networkOps = nullptr,  /* filled in below */
    .modemOps = nullptr,
};

#ifdef __cplusplus
extern "C" {
#endif

const HRilOps *RilInitOps(const struct HRilReport *reportOps)
{
    HR_LOGI("RilInitOps");
    g_hrilOps.modemOps = ModemOps();
    g_hrilOps.simOps = SimOps();
    g_hrilOps.networkOps = NetworkOps();

    /* Must not block: this runs inside riladapter_host's HDF init, long
     * before rild has registered anything.  Start() only spawns the connect
     * thread. */
    RilBridge::Get().Start(reportOps);
    return &g_hrilOps;
}

#ifdef __cplusplus
}
#endif
