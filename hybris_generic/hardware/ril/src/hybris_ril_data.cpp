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
 * dataOps — HRilDataReq mapped onto android.hardware.radio.data v2
 * (plan §5 R5).
 *
 * The data path itself is not ours: the modem terminates the PDP context on
 * a `ccmniX` kernel netdev that lives in the root network namespace, so once
 * setupDataCall succeeds, OHOS's netsys configures that interface exactly as
 * it would on a native device.  All this file does is carry the call setup
 * across the seam and hand back the address/DNS/gateway triplet OHOS needs
 * to do that.
 *
 * Two conversions carry real risk, both in the result:
 *
 *   - OHOS wants ONE space-separated string per field where AIDL has a
 *     vector; CellularDataUtils::ParseIpAddr splits on ' ' and then on '/',
 *     which happens to be exactly LinkAddress::address's own format, so
 *     addresses join verbatim while dns/gateway must not carry a prefix.
 *   - the framework's success test is `reason == 0 && active != 0`
 *     (Activating::RilActivatePdpContextDone).  AIDL spells failure in
 *     DataCallFailCause, whose values are the 3GPP cause codes — the same
 *     numbering as OHOS's PdpErrorReason — so `reason` passes through as an
 *     integer and only the sentinel needs care.
 *
 * What is deliberately NOT implemented is at the bottom, with reasons.
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <aidl/android/hardware/radio/AccessNetwork.h>
#include <aidl/android/hardware/radio/data/ApnTypes.h>
#include <aidl/android/hardware/radio/data/BnRadioDataIndication.h>
#include <aidl/android/hardware/radio/data/BnRadioDataResponse.h>
#include <aidl/android/hardware/radio/data/DataProfileInfo.h>
#include <aidl/android/hardware/radio/data/DataRequestReason.h>
#include <aidl/android/hardware/radio/data/SetupDataCallResult.h>

#include "hril_notification.h"
#include "hril_request.h"
#include "hybris_ril_bridge.h"
#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {
namespace {

namespace rdata = aidl::android::hardware::radio::data;

/* ---- small conversions ----------------------------------------------- */

std::string Join(const std::vector<std::string> &parts, size_t from = 0)
{
    std::string out;
    for (size_t i = from; i < parts.size(); i++) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(parts[i]);
    }
    return out;
}

/*
 * LinkAddress::address is already "addr" or "addr/prefix", which is what
 * CellularDataUtils::ParseIpAddr expects, so this is a join and nothing more.
 * (ParseNormalIpAddr, used for dns and gateway, does NOT strip a prefix — it
 * would end up in the AddressInfo::ip string — which is why those two fields
 * come from vectors of bare addresses and go through Join() untouched.)
 */
std::string JoinAddresses(const std::vector<rdata::LinkAddress> &addresses)
{
    std::string out;
    for (const auto &addr : addresses) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(addr.address);
    }
    return out;
}

/*
 * OHOS carries the PDP type as the string from the APN database
 * (ApnItem::attr_.protocol_), Android as an enum.  The spellings OHOS can
 * produce are the ones telephony_data's parser accepts plus the compiled
 * default "IPV4V6"; anything unrecognised becomes IPV4V6 rather than
 * PdpProtocolType::UNKNOWN, because rild rejects UNKNOWN outright and a
 * dual-stack request is what every modern carrier expects anyway.
 */
rdata::PdpProtocolType ToProtocol(const char *type)
{
    if (type == nullptr || *type == '\0') {
        return rdata::PdpProtocolType::IPV4V6;
    }
    const std::string value(type);
    if (value == "IP" || value == "IPV4") {
        return rdata::PdpProtocolType::IP;
    }
    if (value == "IPV6") {
        return rdata::PdpProtocolType::IPV6;
    }
    if (value == "PPP") {
        return rdata::PdpProtocolType::PPP;
    }
    if (value == "NON-IP" || value == "NONIP") {
        return rdata::PdpProtocolType::NON_IP;
    }
    if (value == "UNSTRUCTURED") {
        return rdata::PdpProtocolType::UNSTRUCTURED;
    }
    return rdata::PdpProtocolType::IPV4V6;
}

/* The reverse, for the type string in the call result.  "IP" rather than
 * "IPV4": that is the 3GPP TS 27.007 spelling and the one OHOS's own APN
 * database uses. */
const char *FromProtocol(rdata::PdpProtocolType protocol)
{
    switch (protocol) {
        case rdata::PdpProtocolType::IP:           return "IP";
        case rdata::PdpProtocolType::IPV6:         return "IPV6";
        case rdata::PdpProtocolType::IPV4V6:       return "IPV4V6";
        case rdata::PdpProtocolType::PPP:          return "PPP";
        case rdata::PdpProtocolType::NON_IP:       return "NON-IP";
        case rdata::PdpProtocolType::UNSTRUCTURED: return "UNSTRUCTURED";
        default:                                   return "";
    }
}

/*
 * HRilDataInfo::rat is an HRilRadioTech (which numbers identically to the
 * framework's RadioTech — it comes straight from GetPsRadioTech), and
 * setupDataCall wants the access network the context should be set up on.
 */
radio::AccessNetwork ToAccessNetwork(int32_t rat)
{
    switch (rat) {
        case RADIO_TECHNOLOGY_GSM:
        case RADIO_TECHNOLOGY_GPRS:
        case RADIO_TECHNOLOGY_EDGE:
            return radio::AccessNetwork::GERAN;
        case RADIO_TECHNOLOGY_WCDMA:
        case RADIO_TECHNOLOGY_HSPA:
        case RADIO_TECHNOLOGY_HSPAP:
        case RADIO_TECHNOLOGY_HSDPA:
        case RADIO_TECHNOLOGY_HSUPA:
        case RADIO_TECHNOLOGY_UMTS:
        case RADIO_TECHNOLOGY_TD_SCDMA:
        case RADIO_TECHNOLOGY_DCHSPAP:
            return radio::AccessNetwork::UTRAN;
        case RADIO_TECHNOLOGY_LTE:
        case RADIO_TECHNOLOGY_LTE_CA:
            return radio::AccessNetwork::EUTRAN;
        case RADIO_TECHNOLOGY_1XRTT:
        case RADIO_TECHNOLOGY_EVDO:
        case RADIO_TECHNOLOGY_EHRPD:
        case RADIO_TECHNOLOGY_IS95A:
        case RADIO_TECHNOLOGY_IS95B:
        case RADIO_TECHNOLOGY_EVDO_0:
        case RADIO_TECHNOLOGY_EVDO_A:
        case RADIO_TECHNOLOGY_EVDO_B:
            return radio::AccessNetwork::CDMA2000;
        case RADIO_TECHNOLOGY_IWLAN:
            return radio::AccessNetwork::IWLAN;
        case RADIO_TECHNOLOGY_NR:
            return radio::AccessNetwork::NGRAN;
        default:
            return radio::AccessNetwork::UNKNOWN;
    }
}

/*
 * ...except that the framework frequently hands us RADIO_TECHNOLOGY_UNKNOWN.
 * GetPsRadioTech reads NetworkState::psRadioTech_, and on this port that
 * field gets reverted to its previous value by
 * NetworkSearchHandler::HandleDelayNotifyEvent -> RevertLastTechnology()
 * shortly after the first registration report; because our registration
 * reports are edge-driven (IRadio only sends networkStateChanged, so we only
 * re-read on a real change) nothing writes it again while the network stays
 * put, and it is left at UNKNOWN indefinitely.
 *
 * rild will not set up a context on AccessNetwork::UNKNOWN, so rather than
 * fail the call, fall back to the RAT the *data* registration itself last
 * reported — which is the same number the framework should have had.
 */
radio::AccessNetwork AccessNetworkFor(int32_t slotId, int32_t rat)
{
    radio::AccessNetwork network = ToAccessNetwork(rat);
    if (network != radio::AccessNetwork::UNKNOWN) {
        return network;
    }
    network = LastDataAccessNetwork(slotId);
    HR_LOGI("framework reported rat=%{public}d; using %{public}d from the data registration",
            rat, static_cast<int32_t>(network));
    return network;
}

/*
 * OHOS's apn-type bitmap (cellular_data ApnTypes) and Android's
 * (radio::data::ApnTypes) agree bit for bit up to XCAP (1 << 11) and then
 * diverge: OHOS spends 1 << 12 on INTERNAL_DEFAULT and 1 << 14 upwards on
 * network slicing, where Android has VSIM and ENTERPRISE.  Pass through only
 * the bits that mean the same thing on both sides, and fold
 * INTERNAL_DEFAULT — a purely OHOS notion for a connection the system itself
 * owns — onto DEFAULT, which is what it is as far as the modem cares.
 */
constexpr int32_t OHOS_APN_TYPES_SHARED_MASK = 0x0FFF;
constexpr int32_t OHOS_APN_TYPE_INTERNAL_DEFAULT = 1 << 12;

int32_t ToApnTypesBitmap(int32_t ohosBitmap)
{
    int32_t bitmap = ohosBitmap & OHOS_APN_TYPES_SHARED_MASK;
    if ((ohosBitmap & OHOS_APN_TYPE_INTERNAL_DEFAULT) != 0) {
        bitmap |= static_cast<int32_t>(rdata::ApnTypes::DEFAULT);
    }
    if (bitmap == 0) {
        bitmap = static_cast<int32_t>(rdata::ApnTypes::DEFAULT);
    }
    return bitmap;
}

/* DataProfileInfo::profileId is an Android-side well-known id, and HRil has
 * no field to carry OHOS's own profile id, so derive it from what the
 * connection is for.  MTK uses it to pick the PDN the context belongs to. */
int32_t ProfileIdFor(int32_t apnTypesBitmap)
{
    if ((apnTypesBitmap & static_cast<int32_t>(rdata::ApnTypes::IMS)) != 0) {
        return rdata::DataProfileInfo::ID_IMS;
    }
    if ((apnTypesBitmap & static_cast<int32_t>(rdata::ApnTypes::DUN)) != 0) {
        return rdata::DataProfileInfo::ID_TETHERED;
    }
    if ((apnTypesBitmap & static_cast<int32_t>(rdata::ApnTypes::FOTA)) != 0) {
        return rdata::DataProfileInfo::ID_FOTA;
    }
    if ((apnTypesBitmap & static_cast<int32_t>(rdata::ApnTypes::CBS)) != 0) {
        return rdata::DataProfileInfo::ID_CBS;
    }
    return rdata::DataProfileInfo::ID_DEFAULT;
}

/*
 * OHOS's DisConnectionReason (cellular_data_constant.h) onto the three
 * reasons IRadioData v2 knows.  Only the handover case is distinguishable;
 * everything else — user turned data off, retry, apn changed — is a normal
 * teardown as far as the modem is concerned.  SHUTDOWN is reserved for the
 * device powering down, which never reaches the vendor RIL through this op.
 */
constexpr int32_t OHOS_DISCONNECT_HANDOVER = 6;

rdata::DataRequestReason ToRequestReason(int32_t reason)
{
    return (reason == OHOS_DISCONNECT_HANDOVER) ? rdata::DataRequestReason::HANDOVER
                                                : rdata::DataRequestReason::NORMAL;
}

rdata::DataProfileInfo BuildProfile(const HRilDataInfo *data, int32_t apnTypesBitmap)
{
    rdata::DataProfileInfo profile;
    profile.apn = (data->apn != nullptr) ? data->apn : "";
    profile.protocol = ToProtocol(data->type);
    profile.roamingProtocol = ToProtocol(data->roamingType);
    profile.authType = static_cast<rdata::ApnAuthType>(data->verType);
    profile.user = (data->userName != nullptr) ? data->userName : "";
    profile.password = (data->password != nullptr) ? data->password : "";
    profile.supportedApnTypesBitmap = apnTypesBitmap;
    profile.profileId = ProfileIdFor(apnTypesBitmap);
    profile.type = rdata::DataProfileInfo::TYPE_3GPP;
    /* 0 = every bearer; OHOS has no per-profile bearer restriction to map. */
    profile.bearerBitmap = 0;
    profile.enabled = true;
    profile.persistent = true;
    profile.preferred = false;
    profile.alwaysOn = false;
    return profile;
}

/*
 * A SetupDataCallResult turned into the C struct hril copies from.
 *
 * HRilDataCallResponse is all `char *`, so the strings have to outlive the
 * report call.  Keeping them in the same object and only taking pointers
 * once the whole list is built means a vector of these can be grown first
 * and pointed at afterwards — taking the pointers as we go would dangle on
 * the next reallocation for every string short enough to live in the SSO
 * buffer.
 */
struct DataCallRecord {
    std::string type;
    std::string ifname;
    std::string address;
    std::string dns;
    std::string dnsSec;
    std::string gateway;
    std::string pcscfPrim;
    std::string pcscfSec;
    int32_t reason = 0;
    int32_t retryTime = 0;
    int32_t cid = 0;
    int32_t active = 0;
    int32_t mtu = 0;
    int32_t pduSessionId = 0;
};

/* AIDL suggestedRetryTime is milliseconds, with -1 for "no suggestion" and
 * INT64_MAX for "never".  OHOS's retryTime is milliseconds too, and
 * ConnectionRetryPolicy treats a negative value as "use my own schedule",
 * so both sentinels collapse to -1. */
constexpr int32_t NO_RETRY_SUGGESTION = -1;

int32_t ToRetryTime(int64_t suggested)
{
    if (suggested < 0 || suggested > INT32_MAX) {
        return NO_RETRY_SUGGESTION;
    }
    return static_cast<int32_t>(suggested);
}

DataCallRecord ToRecord(const rdata::SetupDataCallResult &result)
{
    DataCallRecord record;
    /* DataCallFailCause and PdpErrorReason are both the 3GPP TS 24.008
     * cause codes, down to ERROR_UNSPECIFIED == PDP_ERR_UNKNOWN == 65535. */
    record.reason = static_cast<int32_t>(result.cause);
    record.retryTime = ToRetryTime(result.suggestedRetryTime);
    record.cid = result.cid;
    /* AIDL: 0 inactive, 1 dormant, 2 active.  OHOS documents 0/1 but only
     * ever tests `active != 0` / `active > 0`, so dormant and active both
     * read as up — which is right: a dormant context still has its address. */
    record.active = result.active;
    record.type = FromProtocol(result.type);
    record.ifname = result.ifname;
    record.address = JoinAddresses(result.addresses);
    record.dns = result.dnses.empty() ? "" : result.dnses[0];
    record.dnsSec = Join(result.dnses, 1);
    record.gateway = Join(result.gateways);
    record.pcscfPrim = result.pcscf.empty() ? "" : result.pcscf[0];
    record.pcscfSec = Join(result.pcscf, 1);
    /* One MTU field on the OHOS side; take the v4 one and fall back to v6
     * for an IPv6-only context. */
    record.mtu = (result.mtuV4 != 0) ? result.mtuV4 : result.mtuV6;
    record.pduSessionId = result.pduSessionId;
    return record;
}

HRilDataCallResponse ToResponse(DataCallRecord &record)
{
    HRilDataCallResponse response = {};
    response.reason = record.reason;
    response.retryTime = record.retryTime;
    response.cid = record.cid;
    response.active = record.active;
    response.type = const_cast<char *>(record.type.c_str());
    response.netPortName = const_cast<char *>(record.ifname.c_str());
    response.address = const_cast<char *>(record.address.c_str());
    response.dns = const_cast<char *>(record.dns.c_str());
    response.dnsSec = const_cast<char *>(record.dnsSec.c_str());
    response.gateway = const_cast<char *>(record.gateway.c_str());
    response.maxTransferUnit = record.mtu;
    response.pCscfPrimAddr = const_cast<char *>(record.pcscfPrim.c_str());
    response.pCscfSecAddr = const_cast<char *>(record.pcscfSec.c_str());
    response.pduSessionId = record.pduSessionId;
    return response;
}

/* ---- responses ------------------------------------------------------- */

class DataResponse : public rdata::IRadioDataResponseDefault {
public:
    ::ndk::ScopedAStatus setupDataCallResponse(const radio::RadioResponseInfo &info,
                                               const rdata::SetupDataCallResult &result) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        DataCallRecord record = ToRecord(result);
        HRilDataCallResponse response = ToResponse(record);
        HR_LOGI("setupDataCall: err=%{public}d cause=%{public}d active=%{public}d cid=%{public}d "
                "if=%{public}s addr=%{public}s dns=%{public}s gw=%{public}s mtu=%{public}d",
                static_cast<int32_t>(info.error), record.reason, record.active, record.cid,
                record.ifname.c_str(), record.address.c_str(), record.dns.c_str(),
                record.gateway.c_str(), record.mtu);
        RilBridge::Get().ReportData(req, ToHrilError(static_cast<int32_t>(info.error)), &response,
                                    sizeof(response));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getDataCallListResponse(
        const radio::RadioResponseInfo &info,
        const std::vector<rdata::SetupDataCallResult> &list) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        std::vector<DataCallRecord> records;
        records.reserve(list.size());
        for (const auto &result : list) {
            records.push_back(ToRecord(result));
        }
        std::vector<HRilDataCallResponse> responses;
        responses.reserve(records.size());
        for (auto &record : records) {
            responses.push_back(ToResponse(record));
        }
        HR_LOGI("data call list: err=%{public}d %{public}zu context(s)",
                static_cast<int32_t>(info.error), responses.size());
        /* An empty list is legal here — GetPdpContextListResponse accepts a
         * null pointer as long as the length is 0. */
        RilBridge::Get().ReportData(req, ToHrilError(static_cast<int32_t>(info.error)),
                                    responses.empty() ? nullptr : responses.data(),
                                    responses.size() * sizeof(HRilDataCallResponse));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus deactivateDataCallResponse(const radio::RadioResponseInfo &info) override
    {
        HR_LOGI("deactivateDataCall: err=%{public}d", static_cast<int32_t>(info.error));
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus setInitialAttachApnResponse(const radio::RadioResponseInfo &info) override
    {
        HR_LOGI("setInitialAttachApn: err=%{public}d", static_cast<int32_t>(info.error));
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus setDataProfileResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus setDataAllowedResponse(const radio::RadioResponseInfo &info) override
    {
        HR_LOGI("setDataAllowed: err=%{public}d", static_cast<int32_t>(info.error));
        return ReportEmpty(info);
    }

private:
    ::ndk::ScopedAStatus ReportEmpty(const radio::RadioResponseInfo &info)
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        RilBridge::Get().ReportData(req, ToHrilError(static_cast<int32_t>(info.error)), nullptr, 0);
        return ::ndk::ScopedAStatus::ok();
    }
};

/* ---- indications ----------------------------------------------------- */

class DataIndication : public rdata::IRadioDataIndicationDefault {
public:
    explicit DataIndication(int32_t slotId) : slotId_(slotId) {}

    /*
     * The one indication that matters: it is how the network tearing a
     * context down reaches OHOS at all.  cellular_data compares the list
     * against its own connections and moves any that vanished to Inactive.
     *
     * Unlike GetPdpContextListResponse, HRilData::PdpContextListUpdated
     * rejects a null pointer outright, so an empty list — which is exactly
     * what arrives when the last context drops — has to be reported as a
     * valid pointer with a zero length.
     */
    ::ndk::ScopedAStatus dataCallListChanged(
        radio::RadioIndicationType type,
        const std::vector<rdata::SetupDataCallResult> &list) override
    {
        std::vector<DataCallRecord> records;
        records.reserve(list.size());
        for (const auto &result : list) {
            records.push_back(ToRecord(result));
        }
        std::vector<HRilDataCallResponse> responses;
        responses.reserve(records.size());
        for (auto &record : records) {
            responses.push_back(ToResponse(record));
        }
        HR_LOGI("data call list changed (slot %{public}d): %{public}zu context(s)", slotId_,
                responses.size());
        HRilDataCallResponse empty = {};
        RilBridge::Get().NotifyData(slotId_, HNOTI_DATA_PDP_CONTEXT_LIST_UPDATED,
                                    responses.empty() ? &empty : responses.data(),
                                    responses.size() * sizeof(HRilDataCallResponse));
        AckIndication(type);
        return ::ndk::ScopedAStatus::ok();
    }

    /* Logged and dropped: OHOS has no notification for an APN coming out of
     * throttling (it runs its own retry policy off the setup result), and
     * none for PCO data or a slicing-config change. */
    ::ndk::ScopedAStatus unthrottleApn(radio::RadioIndicationType type,
                                       const rdata::DataProfileInfo &profile) override
    {
        HR_LOGI("unthrottle apn %{public}s (slot %{public}d)", profile.apn.c_str(), slotId_);
        AckIndication(type);
        return ::ndk::ScopedAStatus::ok();
    }

private:
    void AckIndication(radio::RadioIndicationType type)
    {
        if (type != radio::RadioIndicationType::UNSOLICITED_ACK_EXP) {
            return;
        }
        auto data = RilBridge::Get().Data(slotId_);
        if (data != nullptr) {
            data->responseAcknowledgement();
        }
    }

    int32_t slotId_;
};

/* ---- HRilDataReq ----------------------------------------------------- */

/*
 * MTK will not activate a context for an APN it has not been told about.
 *
 * requestSetupDataCall looks the APN up in a table that only setDataProfile
 * fills; miss it and rild logs
 *
 *     [requestSetupDataCall] No APN found from table!! apnIndex=-1, apnCount=0
 *     [requestSetupDataCall] Unknown case.
 *     MIPC_DATA_ACT_CALL_REQ return error, ret=0x150108
 *
 * and answers with cause ERROR_UNSPECIFIED, which tells the framework
 * nothing.  Android's own DataServiceManager pushes the whole carrier
 * profile list at startup and after every APN edit, so rild is never in
 * this state on a stock device.  OHOS has a vendor op for it
 * (HRilDataReq::SetDataProfileInfo) but nothing in cellular_data or
 * core_service ever calls it — SetInitApnInfo is the only profile that ever
 * reaches the RIL, and that is the attach APN, not the table.
 *
 * So we keep the table ourselves: remember every profile we have registered
 * for a slot, and whenever a new one turns up, push the accumulated list
 * before the activation that needs it.  Both calls are oneway on the same
 * binder object, so rild dispatches them in order — the same ordering
 * Android relies on.
 */
std::mutex g_profileLock;
std::vector<rdata::DataProfileInfo> g_profiles[MAX_SLOTS];

bool SameProfile(const rdata::DataProfileInfo &a, const rdata::DataProfileInfo &b)
{
    return a.apn == b.apn && a.protocol == b.protocol &&
           a.supportedApnTypesBitmap == b.supportedApnTypesBitmap && a.authType == b.authType &&
           a.user == b.user && a.password == b.password;
}

void RegisterProfile(int32_t slotId, const rdata::DataProfileInfo &profile,
                     const std::shared_ptr<radio::data::IRadioData> &service)
{
    if (slotId < 0 || slotId >= MAX_SLOTS) {
        return;
    }
    std::vector<rdata::DataProfileInfo> profiles;
    {
        std::lock_guard<std::mutex> guard(g_profileLock);
        auto &known = g_profiles[slotId];
        for (const auto &existing : known) {
            if (SameProfile(existing, profile)) {
                return;
            }
        }
        known.push_back(profile);
        profiles = known;
    }
    HR_LOGI("registering %{public}s with the modem (%{public}zu profile(s) now)",
            profile.apn.c_str(), profiles.size());
    service->setDataProfile(RilBridge::Get().NextSerial(), profiles);
}

void AdoptProfiles(int32_t slotId, const std::vector<rdata::DataProfileInfo> &profiles)
{
    if (slotId < 0 || slotId >= MAX_SLOTS) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_profileLock);
    g_profiles[slotId] = profiles;
}

/*
 * Shared by both activation entry points.  Which one hril calls depends on
 * HRilOps::version: below 13 it is ActivatePdpContext with a bare
 * HRilDataInfo, at 13 and above ActivatePdpContextWithApnTypes, which adds
 * the apn-type bitmap (hril_data.cpp:199).  We declare 13 so the modem is
 * told what the context is for; the older shape stays wired up because hril
 * still routes a V1_1 client through it.
 */
void ActivateInternal(const ReqDataInfo *requestInfo, const HRilDataInfo *data,
                      int32_t ohosApnTypesBitmap)
{
    std::shared_ptr<radio::data::IRadioData> service;
    if (!RilBridge::Get().RequireData(requestInfo, &service)) {
        return;
    }
    if (data == nullptr) {
        HR_LOGE("no data info");
        RilBridge::Get().ReportData(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }

    int32_t apnTypesBitmap = ToApnTypesBitmap(ohosApnTypesBitmap);
    rdata::DataProfileInfo profile = BuildProfile(data, apnTypesBitmap);
    radio::AccessNetwork network = AccessNetworkFor(requestInfo->slotId, data->rat);

    RegisterProfile(requestInfo->slotId, profile, service);

    int32_t serial = RilBridge::Get().Track(requestInfo);
    HR_LOGI("setup data call: apn=%{public}s protocol=%{public}d types=0x%{public}x "
            "network=%{public}d roaming=%{public}d serial=%{public}d",
            profile.apn.c_str(), static_cast<int32_t>(profile.protocol), apnTypesBitmap,
            static_cast<int32_t>(network), data->roamingEnable, serial);

    /* No handover, so no addresses or DNS servers to carry over, and no
     * PDU session to reuse (PDU_SESSION_ID_NOT_SET is 0).  matchAllRuleAllowed
     * lets the modem fall back to a URSP "match all" rule, which is what the
     * framework's own default is. */
    service->setupDataCall(serial, network, profile, data->roamingEnable != 0,
                           rdata::DataRequestReason::NORMAL, {}, {}, 0, std::nullopt, true);
}

void ActivatePdpContext(const ReqDataInfo *requestInfo, const HRilDataInfo *data)
{
    ActivateInternal(requestInfo, data, 0);
}

void ActivatePdpContextWithApnTypes(const ReqDataInfo *requestInfo,
                                    const HRilDataInfoWithApnTypes *data)
{
    if (data == nullptr) {
        HR_LOGE("no data info");
        RilBridge::Get().ReportData(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    /* The two structs share a prefix by construction (hril_data.cpp fills
     * them from the same DataProfileDataInfo), but read the fields rather
     * than casting — the layouts are only conventionally related. */
    HRilDataInfo base = {};
    base.cid = data->cid;
    base.reason = data->reason;
    base.rat = data->rat;
    base.roamingEnable = data->roamingEnable;
    base.verType = data->verType;
    base.userName = data->userName;
    base.password = data->password;
    base.apn = data->apn;
    base.type = data->type;
    base.roamingType = data->roamingType;
    ActivateInternal(requestInfo, &base, data->supportedApnTypesBitmap);
}

void DeactivatePdpContext(const ReqDataInfo *requestInfo, const HRilDataInfo *data)
{
    std::shared_ptr<radio::data::IRadioData> service;
    if (!RilBridge::Get().RequireData(requestInfo, &service)) {
        return;
    }
    if (data == nullptr) {
        HR_LOGE("no data info");
        RilBridge::Get().ReportData(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    HR_LOGI("deactivate data call: cid=%{public}d reason=%{public}d", data->cid, data->reason);
    service->deactivateDataCall(RilBridge::Get().Track(requestInfo), data->cid,
                                ToRequestReason(data->reason));
}

void GetPdpContextList(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::data::IRadioData> service;
    if (!RilBridge::Get().RequireData(requestInfo, &service)) {
        return;
    }
    service->getDataCallList(RilBridge::Get().Track(requestInfo));
}

/*
 * The initial-attach APN: the context the modem brings up by itself as part
 * of attaching to the PS domain, before anything asks for a data call.  MTK
 * cares — without it the modem attaches with no PDN and the first
 * setupDataCall has to do the whole thing from cold.
 */
void SetInitApnInfo(const ReqDataInfo *requestInfo, const HRilDataInfo *data)
{
    std::shared_ptr<radio::data::IRadioData> service;
    if (!RilBridge::Get().RequireData(requestInfo, &service)) {
        return;
    }
    if (data == nullptr) {
        HR_LOGE("no data info");
        RilBridge::Get().ReportData(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    /* IA is the apn type for an initial-attach profile; DEFAULT alongside it
     * because that is what the same APN will be used for. */
    int32_t apnTypesBitmap = static_cast<int32_t>(rdata::ApnTypes::IA) |
                             static_cast<int32_t>(rdata::ApnTypes::DEFAULT);
    rdata::DataProfileInfo profile = BuildProfile(data, apnTypesBitmap);
    profile.profileId = rdata::DataProfileInfo::ID_DEFAULT;
    HR_LOGI("initial attach apn: %{public}s protocol=%{public}d", profile.apn.c_str(),
            static_cast<int32_t>(profile.protocol));
    /* The framework sends this before anything else, so it is also the first
     * chance to prime MTK's profile table — see RegisterProfile(). */
    RegisterProfile(requestInfo->slotId, profile, service);
    service->setInitialAttachApn(RilBridge::Get().Track(requestInfo), profile);
}

/*
 * `len` is a count of profiles, not bytes — HRilData::SetDataProfileInfo
 * passes dataProfilesInfo.profilesSize.
 */
void SetDataProfileInfo(const ReqDataInfo *requestInfo, const HRilDataInfo *data, int32_t len)
{
    std::shared_ptr<radio::data::IRadioData> service;
    if (!RilBridge::Get().RequireData(requestInfo, &service)) {
        return;
    }
    if (data == nullptr || len <= 0) {
        HR_LOGE("no profiles");
        RilBridge::Get().ReportData(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    std::vector<rdata::DataProfileInfo> profiles;
    profiles.reserve(static_cast<size_t>(len));
    for (int32_t i = 0; i < len; i++) {
        /* No apn types in this struct either; DEFAULT is the only thing
         * OHOS ever puts in the profile list today. */
        profiles.push_back(BuildProfile(&data[i], static_cast<int32_t>(rdata::ApnTypes::DEFAULT)));
    }
    HR_LOGI("set %{public}d data profile(s)", len);
    /* Nothing in this tree calls this op, but if something ever does it owns
     * the table outright — adopt its list so RegisterProfile() does not push
     * a stale one over the top. */
    AdoptProfiles(requestInfo->slotId, profiles);
    service->setDataProfile(RilBridge::Get().Track(requestInfo), profiles);
}

/* Whether this SIM is allowed to use packet data at all — the framework's
 * lever for the "which slot owns data" choice, and for the temporary grant
 * that lets an MMS go out over the non-default slot. */
void SetDataPermitted(const ReqDataInfo *requestInfo, int32_t enabled)
{
    std::shared_ptr<radio::data::IRadioData> service;
    if (!RilBridge::Get().RequireData(requestInfo, &service)) {
        return;
    }
    HR_LOGI("data permitted: %{public}d", enabled);
    service->setDataAllowed(RilBridge::Get().Track(requestInfo), enabled != 0);
}

/*
 * Left NULL on purpose:
 *
 *   GetLinkBandwidthInfo / SetLinkBandwidthReportingRule / GetLinkCapability
 *     IRadioData has no equivalent — Android reports link capacity through
 *     IRadioNetwork's setLinkCapacityReportingCriteria and its own
 *     indications, which OHOS does not consume.  The one consumer of the
 *     answer, CellularDataStateMachine::SetBandwidth, has no caller in this
 *     tree, so the values would go into NetSupplierInfo's scoring fields and
 *     nowhere else.  Inventing them is worse than the zeros that mean
 *     "unknown".
 *
 *   CleanAllConnections
 *     Would have to be a deactivate loop over getDataCallList, i.e. a
 *     multi-request state machine whose partial failure leaves the framework
 *     believing every context is gone.  cellular_data only issues this on
 *     paths where it also tears the contexts down individually.
 *
 *   SendDataPerformanceMode / SendDataSleepMode
 *     Huawei-modem power hints with no AIDL counterpart.
 *
 *   The UE-policy, NSSAI and slice ops (SendUrspDecodeResult,
 *   SendUePolicySectionIdentifier, SendImsRsdList, GetNetworkSliceAllowedNssai,
 *   GetNetworkSliceEhplmn, ActivatePdpContextWithApnTypesforSlice)
 *     5G network slicing.  IS_SUPPORT_NR_SLICE is off in this product, the
 *     device is not on a slicing network, and none of it is testable here.
 */
const HRilDataReq g_dataOps = {
    .SetInitApnInfo = SetInitApnInfo,
    .ActivatePdpContext = ActivatePdpContext,
    .DeactivatePdpContext = DeactivatePdpContext,
    .GetPdpContextList = GetPdpContextList,
    .GetLinkBandwidthInfo = nullptr,
    .SetLinkBandwidthReportingRule = nullptr,
    .SetDataProfileInfo = SetDataProfileInfo,
    .SendDataPerformanceMode = nullptr,
    .SendDataSleepMode = nullptr,
    .SetDataPermitted = SetDataPermitted,
    .GetLinkCapability = nullptr,
    .CleanAllConnections = nullptr,
    .ActivatePdpContextWithApnTypes = ActivatePdpContextWithApnTypes,
    .SendUrspDecodeResult = nullptr,
    .SendUePolicySectionIdentifier = nullptr,
    .SendImsRsdList = nullptr,
    .GetNetworkSliceAllowedNssai = nullptr,
    .GetNetworkSliceEhplmn = nullptr,
    .ActivatePdpContextWithApnTypesforSlice = nullptr,
};

} // namespace

const HRilDataReq *DataOps()
{
    return &g_dataOps;
}

void AttachDataCallbacks(int32_t slotId, const std::shared_ptr<radio::data::IRadioData> &data)
{
    static std::shared_ptr<rdata::IRadioDataResponseDelegator> resp[MAX_SLOTS];
    static std::shared_ptr<rdata::IRadioDataIndicationDelegator> ind[MAX_SLOTS];

    resp[slotId] = ::ndk::SharedRefBase::make<rdata::IRadioDataResponseDelegator>(
        ::ndk::SharedRefBase::make<DataResponse>());
    ind[slotId] = ::ndk::SharedRefBase::make<rdata::IRadioDataIndicationDelegator>(
        ::ndk::SharedRefBase::make<DataIndication>(slotId));

    auto st = data->setResponseFunctions(resp[slotId], ind[slotId]);
    if (!st.isOk()) {
        HR_LOGE("data setResponseFunctions: %{public}s", st.getDescription().c_str());
    }
}

} // namespace HybrisRil
} // namespace OHOS
