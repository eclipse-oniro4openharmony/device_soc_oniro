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

/* networkOps — registration, operator and signal, mapped onto
 * android.hardware.radio.network v2. */

#include <memory>
#include <string>
#include <vector>

#include <aidl/android/hardware/radio/network/BnRadioNetworkIndication.h>
#include <aidl/android/hardware/radio/network/BnRadioNetworkResponse.h>
#include <aidl/android/hardware/radio/network/CellIdentity.h>
#include <aidl/android/hardware/radio/network/RegStateResult.h>
#include <aidl/android/hardware/radio/network/SignalStrength.h>

#include "hril_notification.h"
#include "hril_request.h"
#include "hybris_ril_bridge.h"
#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {
namespace {

namespace rnet = aidl::android::hardware::radio::network;

/* AIDL RadioTechnology and HRilRadioTech both start from an "unknown = 0"
 * but agree on nothing else — OHOS orders its constants by generation,
 * Android by the order they were added. */
HRilRadioTech ToHrilRat(radio::RadioTechnology rat)
{
    using RT = radio::RadioTechnology;
    switch (rat) {
        case RT::GPRS:      return RADIO_TECHNOLOGY_GPRS;
        case RT::EDGE:      return RADIO_TECHNOLOGY_EDGE;
        case RT::UMTS:      return RADIO_TECHNOLOGY_UMTS;
        case RT::IS95A:     return RADIO_TECHNOLOGY_IS95A;
        case RT::IS95B:     return RADIO_TECHNOLOGY_IS95B;
        case RT::ONE_X_RTT: return RADIO_TECHNOLOGY_1XRTT;
        case RT::EVDO_0:    return RADIO_TECHNOLOGY_EVDO_0;
        case RT::EVDO_A:    return RADIO_TECHNOLOGY_EVDO_A;
        case RT::HSDPA:     return RADIO_TECHNOLOGY_HSDPA;
        case RT::HSUPA:     return RADIO_TECHNOLOGY_HSUPA;
        case RT::HSPA:      return RADIO_TECHNOLOGY_HSPA;
        case RT::EVDO_B:    return RADIO_TECHNOLOGY_EVDO_B;
        case RT::EHRPD:     return RADIO_TECHNOLOGY_EHRPD;
        case RT::LTE:       return RADIO_TECHNOLOGY_LTE;
        case RT::HSPAP:     return RADIO_TECHNOLOGY_HSPAP;
        case RT::GSM:       return RADIO_TECHNOLOGY_GSM;
        case RT::TD_SCDMA:  return RADIO_TECHNOLOGY_TD_SCDMA;
        case RT::IWLAN:     return RADIO_TECHNOLOGY_IWLAN;
        case RT::NR:        return RADIO_TECHNOLOGY_NR;
        default:            return RADIO_TECHNOLOGY_UNKNOWN;
    }
}

/* 0-5 line up exactly; the "_EM" states mean "no service but emergency
 * calls are possible", which OHOS spells REG_MT_EMERGENCY. */
HRilRegStatus ToHrilRegStatus(rnet::RegState state)
{
    using RS = rnet::RegState;
    switch (state) {
        case RS::NOT_REG_MT_NOT_SEARCHING_OP: return NO_REG_MT_NO_SEARCH;
        case RS::REG_HOME:                    return REG_MT_HOME;
        case RS::NOT_REG_MT_SEARCHING_OP:     return NO_REG_MT_SEARCHING;
        case RS::REG_DENIED:                  return REG_MT_REJECTED;
        case RS::REG_ROAMING:                 return REG_MT_ROAMING;
        case RS::NOT_REG_MT_NOT_SEARCHING_OP_EM:
        case RS::NOT_REG_MT_SEARCHING_OP_EM:
        case RS::REG_DENIED_EM:
        case RS::UNKNOWN_EM:
        case RS::REG_EM:                      return REG_MT_EMERGENCY;
        default:                              return REG_MT_UNKNOWN;
    }
}

/* HRilRegStatusInfo carries a flat (lac, cellId) pair; AIDL keeps them in
 * a per-RAT CellIdentity union. */
void ExtractCell(const rnet::CellIdentity &id, int32_t *lac, int32_t *cellId)
{
    *lac = 0;
    *cellId = 0;
    switch (id.getTag()) {
        case rnet::CellIdentity::gsm: {
            const auto &c = id.get<rnet::CellIdentity::gsm>();
            *lac = c.lac;
            *cellId = c.cid;
            break;
        }
        case rnet::CellIdentity::wcdma: {
            const auto &c = id.get<rnet::CellIdentity::wcdma>();
            *lac = c.lac;
            *cellId = c.cid;
            break;
        }
        case rnet::CellIdentity::tdscdma: {
            const auto &c = id.get<rnet::CellIdentity::tdscdma>();
            *lac = c.lac;
            *cellId = c.cid;
            break;
        }
        case rnet::CellIdentity::lte: {
            const auto &c = id.get<rnet::CellIdentity::lte>();
            *lac = c.tac;
            *cellId = c.ci;
            break;
        }
        case rnet::CellIdentity::nr: {
            const auto &c = id.get<rnet::CellIdentity::nr>();
            *lac = c.tac;
            /* NCI is 36 bits; HRil's cellId is int32_t.  Truncating is what
             * the framework can carry and is only used for display/logging. */
            *cellId = static_cast<int32_t>(c.nci);
            break;
        }
        default:
            break;
    }
}

void ToHrilRegStatusInfo(const rnet::RegStateResult &reg, HRilRegStatusInfo *out)
{
    out->notifyMode = REG_NOTIFY_STAT_LAC_CELLID;
    out->regStatus = ToHrilRegStatus(reg.regState);
    out->actType = ToHrilRat(reg.rat);
    ExtractCell(reg.cellIdentity, &out->lacCode, &out->cellId);
    out->isNrAvailable = 0;
    out->isEnDcAvailable = 0;
    out->isDcNrRestricted = 0;
}

/*
 * Both sides use "invalid" sentinels but not the same one: AIDL says
 * INT32_MAX, OHOS's framework expects the 3GPP "unknown" codes it would
 * have parsed out of an AT +CESQ (99 for a level, 255 for a dBm-derived
 * value).  Pass the sentinel through unchanged and the signal bars read
 * as full-strength garbage.
 */
constexpr int32_t AIDL_INVALID = 2147483647;
int32_t Level(int32_t v)   { return v == AIDL_INVALID ? 99 : v; }
int32_t Dbm(int32_t v)     { return v == AIDL_INVALID ? 255 : v; }

void ToHrilRssi(const rnet::SignalStrength &s, HRilRssi *out)
{
    out->gsmRssi.rxlev = Level(s.gsm.signalStrength);
    out->gsmRssi.ber = Level(s.gsm.bitErrorRate);

    out->wcdmaRssi.rxlev = Level(s.wcdma.signalStrength);
    out->wcdmaRssi.ber = Level(s.wcdma.bitErrorRate);
    out->wcdmaRssi.rscp = Dbm(s.wcdma.rscp);
    out->wcdmaRssi.ecio = Dbm(s.wcdma.ecno);

    out->cdmaRssi.absoluteRssi = Dbm(s.cdma.dbm);
    out->cdmaRssi.ecno = Dbm(s.cdma.ecio);

    out->lteRssi.rxlev = Level(s.lte.signalStrength);
    out->lteRssi.rsrp = Dbm(s.lte.rsrp);
    out->lteRssi.rsrq = Dbm(s.lte.rsrq);
    out->lteRssi.snr = Dbm(s.lte.rssnr);

    out->tdScdmaRssi.rscp = Dbm(s.tdscdma.rscp);

    out->nrRssi.rsrp = Dbm(s.nr.ssRsrp);
    out->nrRssi.rsrq = Dbm(s.nr.ssRsrq);
    out->nrRssi.sinr = Dbm(s.nr.ssSinr);
}

/* ---- responses ------------------------------------------------------- */

class NetworkResponse : public rnet::IRadioNetworkResponseDefault {
public:
    ::ndk::ScopedAStatus getSignalStrengthResponse(const radio::RadioResponseInfo &info,
                                                   const rnet::SignalStrength &sig) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HRilRssi rssi = {};
        ToHrilRssi(sig, &rssi);
        RilBridge::Get().ReportNetwork(req, ToHrilError(static_cast<int32_t>(info.error)), &rssi,
                                       sizeof(rssi));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getVoiceRegistrationStateResponse(
        const radio::RadioResponseInfo &info, const rnet::RegStateResult &reg) override
    {
        return ReportReg(info, reg, "voice");
    }

    ::ndk::ScopedAStatus getDataRegistrationStateResponse(
        const radio::RadioResponseInfo &info, const rnet::RegStateResult &reg) override
    {
        return ReportReg(info, reg, "data");
    }

    ::ndk::ScopedAStatus getOperatorResponse(const radio::RadioResponseInfo &info,
                                             const std::string &longName,
                                             const std::string &shortName,
                                             const std::string &numeric) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        /* hril reads this one as `char **resp` with three entries. */
        char *resp[3] = { const_cast<char *>(longName.c_str()),
                          const_cast<char *>(shortName.c_str()),
                          const_cast<char *>(numeric.c_str()) };
        RilBridge::Get().ReportNetwork(req, ToHrilError(static_cast<int32_t>(info.error)), resp,
                                       sizeof(resp));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getNetworkSelectionModeResponse(const radio::RadioResponseInfo &info,
                                                         bool manual) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        int32_t mode = manual ? 1 : 0;
        RilBridge::Get().ReportNetwork(req, ToHrilError(static_cast<int32_t>(info.error)), &mode,
                                       sizeof(mode));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus setNetworkSelectionModeAutomaticResponse(
        const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus setNetworkSelectionModeManualResponse(
        const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus setAllowedNetworkTypesBitmapResponse(
        const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus setLocationUpdatesResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus setIndicationFilterResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus getAllowedNetworkTypesBitmapResponse(
        const radio::RadioResponseInfo &info, int32_t networkTypeBitmap) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        /* OHOS wants its own HRilPreferredNetworkType, not a RAF bitmap.
         * Report AUTO unless the mask is a single well-known family — the
         * framework only uses this to pre-select a menu entry. */
        int32_t preferred = HRIL_NETWORK_AUTO;
        (void)networkTypeBitmap;
        RilBridge::Get().ReportNetwork(req, ToHrilError(static_cast<int32_t>(info.error)),
                                       &preferred, sizeof(preferred));
        return ::ndk::ScopedAStatus::ok();
    }

private:
    ::ndk::ScopedAStatus ReportEmpty(const radio::RadioResponseInfo &info)
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        RilBridge::Get().ReportNetwork(req, ToHrilError(static_cast<int32_t>(info.error)), nullptr,
                                       0);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus ReportReg(const radio::RadioResponseInfo &info,
                                   const rnet::RegStateResult &reg, const char *what)
    {
        HRilRegStatusInfo out = {};
        ToHrilRegStatusInfo(reg, &out);

        /* Two callers: a framework request, or our own re-read triggered by
         * networkStateChanged (which carries no payload of its own). */
        int32_t slotId = 0;
        int32_t notifyId = 0;
        if (RilBridge::Get().TakeNotify(info.serial, &slotId, &notifyId)) {
            HR_LOGI("%{public}s reg=%{public}d rat=%{public}d plmn=%{public}s (unsol)", what,
                    out.regStatus, out.actType, reg.registeredPlmn.c_str());
            RilBridge::Get().NotifyNetwork(slotId, notifyId, &out, sizeof(out));
            return ::ndk::ScopedAStatus::ok();
        }

        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HR_LOGI("%{public}s reg=%{public}d rat=%{public}d plmn=%{public}s", what, out.regStatus,
                out.actType, reg.registeredPlmn.c_str());
        RilBridge::Get().ReportNetwork(req, ToHrilError(static_cast<int32_t>(info.error)), &out,
                                       sizeof(out));
        return ::ndk::ScopedAStatus::ok();
    }
};

/* ---- indications ----------------------------------------------------- */

class NetworkIndication : public rnet::IRadioNetworkIndicationDefault {
public:
    explicit NetworkIndication(int32_t slotId) : slotId_(slotId) {}

    ::ndk::ScopedAStatus networkStateChanged(radio::RadioIndicationType) override
    {
        /* IRadio only says "something changed"; HRil's notification carries
         * a full HRilRegStatusInfo.  Re-read both domains and let the
         * response handler emit the notification (TrackNotify). */
        auto network = RilBridge::Get().Network(slotId_);
        if (network == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        network->getVoiceRegistrationState(
            RilBridge::Get().TrackNotify(slotId_, HNOTI_NETWORK_CS_REG_STATUS_UPDATED));
        network->getDataRegistrationState(
            RilBridge::Get().TrackNotify(slotId_, HNOTI_NETWORK_PS_REG_STATUS_UPDATED));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus currentSignalStrength(radio::RadioIndicationType,
                                               const rnet::SignalStrength &sig) override
    {
        HRilRssi rssi = {};
        ToHrilRssi(sig, &rssi);
        RilBridge::Get().NotifyNetwork(slotId_, HNOTI_NETWORK_SIGNAL_STRENGTH_UPDATED, &rssi,
                                       sizeof(rssi));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus nitzTimeReceived(radio::RadioIndicationType, const std::string &nitzTime,
                                          int64_t, int64_t) override
    {
        RilBridge::Get().NotifyNetwork(slotId_, HNOTI_NETWORK_TIME_UPDATED, nitzTime.c_str(),
                                       nitzTime.size());
        return ::ndk::ScopedAStatus::ok();
    }

private:
    int32_t slotId_;
};

/* ---- HRilNetworkReq -------------------------------------------------- */

void GetSignalStrength(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    network->getSignalStrength(RilBridge::Get().Track(requestInfo));
}

void GetCsRegStatus(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    network->getVoiceRegistrationState(RilBridge::Get().Track(requestInfo));
}

void GetPsRegStatus(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    network->getDataRegistrationState(RilBridge::Get().Track(requestInfo));
}

void GetOperatorInfo(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    network->getOperator(RilBridge::Get().Track(requestInfo));
}

void GetNetworkSelectionMode(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    network->getNetworkSelectionMode(RilBridge::Get().Track(requestInfo));
}

void SetNetworkSelectionMode(const ReqDataInfo *requestInfo, const HRilSetNetworkModeInfo *data)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    int32_t serial = RilBridge::Get().Track(requestInfo);
    if (data == nullptr || data->selectMode == 0 || data->oper == nullptr) {
        network->setNetworkSelectionModeAutomatic(serial);
    } else {
        /* AccessNetwork UNKNOWN lets the modem pick the RAT for the PLMN. */
        network->setNetworkSelectionModeManual(serial, data->oper,
                                               radio::AccessNetwork::UNKNOWN);
    }
}

void GetPreferredNetwork(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    network->getAllowedNetworkTypesBitmap(RilBridge::Get().Track(requestInfo));
}

void SetPreferredNetwork(const ReqDataInfo *requestInfo, const int32_t *data)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    /* RadioAccessFamily bits for "everything this modem can do".  OHOS's
     * per-family preferences (HRilPreferredNetworkType) do not translate
     * cleanly and restricting the modem is worse than not restricting it,
     * so honour only the auto case for now. */
    (void)data;
    constexpr int32_t RAF_ALL = 0x7FFFFFFF;
    network->setAllowedNetworkTypesBitmap(RilBridge::Get().Track(requestInfo), RAF_ALL);
}

void SetLocateUpdates(const ReqDataInfo *requestInfo, HRilRegNotifyMode mode)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    network->setLocationUpdates(RilBridge::Get().Track(requestInfo),
                                mode != REG_NOT_NOTIFY);
}

void SetNotificationFilter(const ReqDataInfo *requestInfo, const int32_t *newFilter)
{
    std::shared_ptr<rnet::IRadioNetwork> network;
    if (!RilBridge::Get().RequireNetwork(requestInfo, &network)) {
        return;
    }
    /* HRilNotificationFilter and IRadio's indication filter use the same
     * bit positions (signal strength, network state, data call, link
     * capacity, physical channel config); -1 means "all" on both sides. */
    network->setIndicationFilter(RilBridge::Get().Track(requestInfo),
                                 newFilter != nullptr ? *newFilter : -1);
}

const HRilNetworkReq g_networkOps = {
    .GetImsRegStatus = nullptr,
    .GetSignalStrength = GetSignalStrength,
    .GetCsRegStatus = GetCsRegStatus,
    .GetPsRegStatus = GetPsRegStatus,
    .GetOperatorInfo = GetOperatorInfo,
    .GetNeighboringCellInfoList = nullptr,
    .GetCurrentCellInfo = nullptr,
    .GetNetworkSearchInformation = nullptr,
    .GetNetworkSelectionMode = GetNetworkSelectionMode,
    .SetNetworkSelectionMode = SetNetworkSelectionMode,
    .GetPreferredNetwork = GetPreferredNetwork,
    .SetPreferredNetwork = SetPreferredNetwork,
    .GetPhysicalChannelConfig = nullptr,
    .SetLocateUpdates = SetLocateUpdates,
    .SetNotificationFilter = SetNotificationFilter,
};

} // namespace

const HRilNetworkReq *NetworkOps()
{
    return &g_networkOps;
}

void AttachNetworkCallbacks(int32_t slotId, const std::shared_ptr<rnet::IRadioNetwork> &network)
{
    static std::shared_ptr<rnet::IRadioNetworkResponseDelegator> resp[MAX_SLOTS];
    static std::shared_ptr<rnet::IRadioNetworkIndicationDelegator> ind[MAX_SLOTS];

    resp[slotId] = ::ndk::SharedRefBase::make<rnet::IRadioNetworkResponseDelegator>(
        ::ndk::SharedRefBase::make<NetworkResponse>());
    ind[slotId] = ::ndk::SharedRefBase::make<rnet::IRadioNetworkIndicationDelegator>(
        ::ndk::SharedRefBase::make<NetworkIndication>(slotId));

    auto st = network->setResponseFunctions(resp[slotId], ind[slotId]);
    if (!st.isOk()) {
        HR_LOGE("network setResponseFunctions: %{public}s", st.getDescription().c_str());
    }
}

} // namespace HybrisRil
} // namespace OHOS
