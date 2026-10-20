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
 * AGNSS_MODULE_INTERFACE: the OHOS AGNSS HDI (agnss_interface_service) onto
 * the HAL's IAGnss (SUPL server, data link) and IAGnssRil (set ID, cell
 * reference location, network state).
 */

#include <cstring>
#include <string>

#include "hybris_gnss_bridge.h"
#include "hybris_gnss_log.h"

namespace OHOS {
namespace HybrisGnss {
namespace {

/* The HDI may set these before the HAL is reachable (its AGNSS device loads
 * with the GNSS one); the RIL calls are hints, so a missed early one is
 * simply superseded by the next.  Only the callback table must survive. */
constexpr int32_t AGNSS_CONNECT_WAIT_MS = 2000;

bool SetAgnssCallback(AgnssCallbackIfaces *callbacks)
{
    if (callbacks == nullptr) {
        return false;
    }
    GnssBridge::Get().SetAgnssCallbacks(*callbacks);
    GnssBridge::Get().Start();
    return true;
}

gnss::IAGnssRil::AGnssRefLocationType ToAidlCellType(uint16_t category)
{
    switch (static_cast<CellIdCategory>(category)) {
        case CellIdCategory::CELLID_CATEGORY_UMTS:
            return gnss::IAGnssRil::AGnssRefLocationType::UMTS_CELLID;
        case CellIdCategory::CELLID_CATEGORY_LTE:
            return gnss::IAGnssRil::AGnssRefLocationType::LTE_CELLID;
        case CellIdCategory::CELLID_CATEGORY_NR:
            return gnss::IAGnssRil::AGnssRefLocationType::NR_CELLID;
        default:
            return gnss::IAGnssRil::AGnssRefLocationType::GSM_CELLID;
    }
}

bool SetAgnssReferenceInfo(const AgnssReferenceInfo *refInfo)
{
    if (refInfo == nullptr) {
        return false;
    }
    /* IAGnssRil carries only cell references; a WiFi MAC has no seam. */
    if (refInfo->category != static_cast<uint32_t>(AgnssRefInfoCategory::AGNSS_REF_INFO_CATEGORY_CELLID)) {
        return true;
    }
    (void)GnssBridge::Get().WaitConnected(AGNSS_CONNECT_WAIT_MS);
    auto ril = GnssBridge::Get().AGnssRil();
    if (ril == nullptr) {
        return false;
    }
    const CellId &in = refInfo->u.cellId;
    gnss::IAGnssRil::AGnssRefLocation ref;
    ref.type = ToAidlCellType(in.category);
    ref.cellID.type = ref.type;
    ref.cellID.mcc = in.mcc;
    ref.cellID.mnc = in.mnc;
    ref.cellID.lac = in.lac;
    ref.cellID.cid = (ref.type == gnss::IAGnssRil::AGnssRefLocationType::NR_CELLID)
        ? static_cast<int64_t>(in.nci) : static_cast<int64_t>(in.cid);
    ref.cellID.tac = in.tac;
    ref.cellID.pcid = in.pcid;
    ref.cellID.arfcn = 0;
    auto st = ril->setRefLocation(ref);
    if (!st.isOk()) {
        HG_LOGW("IAGnssRil.setRefLocation: %{public}s", st.getDescription().c_str());
        return false;
    }
    return true;
}

bool SetSetid(uint16_t category, const char *setid, size_t length)
{
    (void)GnssBridge::Get().WaitConnected(AGNSS_CONNECT_WAIT_MS);
    auto ril = GnssBridge::Get().AGnssRil();
    if (ril == nullptr) {
        return false;
    }
    gnss::IAGnssRil::SetIdType type;
    switch (static_cast<AgnssSetidCategory>(category)) {
        case AgnssSetidCategory::AGNSS_SETID_CATEGORY_IMSI:
            type = gnss::IAGnssRil::SetIdType::IMSI;
            break;
        case AgnssSetidCategory::AGNSS_SETID_CATEGORY_MSISDN:
            type = gnss::IAGnssRil::SetIdType::MSISDM;
            break;
        default:
            type = gnss::IAGnssRil::SetIdType::NONE;
            break;
    }
    std::string id = (setid != nullptr) ? std::string(setid, length) : std::string();
    auto st = ril->setSetId(type, id);
    if (!st.isOk()) {
        HG_LOGW("IAGnssRil.setSetId: %{public}s", st.getDescription().c_str());
        return false;
    }
    return true;
}

bool SetAgnssServer(uint16_t category, const char *server, size_t length, int32_t port)
{
    (void)GnssBridge::Get().WaitConnected(AGNSS_CONNECT_WAIT_MS);
    auto agnss = GnssBridge::Get().AGnss();
    if (agnss == nullptr || server == nullptr) {
        return false;
    }
    /* AgnssCategory SUPL/C2K = AGnssType SUPL/C2K. */
    auto type = static_cast<gnss::IAGnssCallback::AGnssType>(category);
    std::string host(server, length);
    auto st = agnss->setServer(type, host, port);
    if (!st.isOk()) {
        HG_LOGW("IAGnss.setServer: %{public}s", st.getDescription().c_str());
        return false;
    }
    HG_LOGI("SUPL server %{public}s:%{public}d", host.c_str(), port);
    return true;
}

void OnNetworkStateChange(const NetworkState *state)
{
    if (state == nullptr) {
        return;
    }
    auto ril = GnssBridge::Get().AGnssRil();
    if (ril == nullptr) {
        return;
    }
    gnss::IAGnssRil::NetworkAttributes attrs;
    attrs.networkHandle = state->netId;
    attrs.isConnected =
        (state->state == static_cast<uint8_t>(DataConnectionState::DATA_CONNECTION_CONNECTED));
    attrs.capabilities = 0;
    attrs.apn = std::string(state->apn, strnlen(state->apn, APN_LENGTH_MAXIMUM));
    auto st = ril->updateNetworkState(attrs);
    if (!st.isOk()) {
        HG_LOGW("IAGnssRil.updateNetworkState: %{public}s", st.getDescription().c_str());
    }
}

const AgnssModuleInterface g_agnssModule = {
    .size = sizeof(AgnssModuleInterface),
    .setAgnssCallback = SetAgnssCallback,
    .setAgnssReferenceInfo = SetAgnssReferenceInfo,
    .setSetid = SetSetid,
    .setAgnssServer = SetAgnssServer,
    .onNetworkStateChange = OnNetworkStateChange,
};

} // namespace

const AgnssModuleInterface *AgnssModule()
{
    return &g_agnssModule;
}

} // namespace HybrisGnss
} // namespace OHOS
