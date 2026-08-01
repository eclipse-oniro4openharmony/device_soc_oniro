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
 * callOps — HRilCallReq mapped onto android.hardware.radio.voice v2
 * (plan §5 R6).
 *
 * IRadioVoice is the circuit-switched domain and nothing else: the IMS calls
 * that OHOS reaches through GetImsCallList and the ims_service SA live behind
 * IRadioIms, which is plan phase R8.  So everything here is a CS call, and
 * HRilCallInfo::voiceDomain is always 0.
 *
 * Both sides descend from the same AT commands, which makes the mapping
 * unusually direct — 3GPP TS 27.007 is the shared ancestor.  Where OHOS's
 * vendor ABI documents an AT command and AIDL documents a RIL request, the
 * two agree; base/telephony/ril_adapter's own AT sample (at_call.c) is the
 * cross-check used throughout:
 *
 *     Dial              ATD<num>;      -> dial
 *     Answer            ATA            -> acceptCall
 *     Reject            ATH            -> rejectCall            (UDUB)
 *     Hangup(index)     AT+CHLD=1<idx> -> hangup(gsmIndex)
 *     Hold/UnHold/Switch AT+CHLD=2     -> switchWaitingOrHoldingAndActive
 *     CombineConference AT+CHLD=3      -> conference
 *     SeparateConference AT+CHLD=2<idx>-> separateConnection(gsmIndex)
 *     CallSupplement 0  AT+CHLD=0      -> hangupWaitingOrBackground
 *     CallSupplement 1  AT+CHLD=1      -> hangupForegroundResumeBackground
 *     GetCallList       AT+CLCC        -> getCurrentCalls
 *     GetCallFailReason AT+CEER        -> getLastCallFailCause
 *
 * The call states are the +CLCC <stat> numbering on both sides (HRilCallState
 * against voice::Call::STATE_*), so they pass through as integers.
 *
 * What is deliberately NOT implemented is at the bottom, with reasons.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <aidl/android/hardware/radio/voice/BnRadioVoiceIndication.h>
#include <aidl/android/hardware/radio/voice/BnRadioVoiceResponse.h>
#include <aidl/android/hardware/radio/voice/Call.h>
#include <aidl/android/hardware/radio/voice/Dial.h>
#include <aidl/android/hardware/radio/voice/EmergencyNumber.h>
#include <aidl/android/hardware/radio/voice/LastCallFailCauseInfo.h>

#include "hril_notification.h"
#include "hril_request.h"
#include "hybris_ril_bridge.h"
#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {
namespace {

namespace rvoice = aidl::android::hardware::radio::voice;

/* ---- the call list --------------------------------------------------- */

/*
 * One AIDL Call turned into the C struct hril copies from.
 *
 * HRilCallInfo carries three `char *`, so the strings have to outlive the
 * report.  Same shape as the data domain's DataCallRecord and for the same
 * reason: build every record first, take the pointers only once the vector
 * has stopped growing, or a reallocation dangles every string short enough
 * to have lived in its own SSO buffer.
 */
struct CallRecord {
    std::string number;
    std::string name;
    int32_t index = 0;
    int32_t dir = 0;
    int32_t state = 0;
    int32_t mode = 0;
    int32_t mpty = 0;
    int32_t type = 0;
    int32_t namePresentation = 0;
};

CallRecord ToRecord(const rvoice::Call &call)
{
    CallRecord record;
    record.number = call.number;
    record.name = call.name;
    record.index = call.index;
    /* HRilCallInfo::dir is 0 mobile-originated, 1 mobile-terminated. */
    record.dir = call.isMT ? 1 : 0;
    /* Both sides number the +CLCC <stat> field identically: 0 active,
     * 1 held, 2 dialing, 3 alerting, 4 incoming, 5 waiting. */
    record.state = call.state;
    /* HRilCallMode: 0 voice, 1 data, 2 fax.  AIDL only distinguishes voice
     * from not-voice; a CS data call is the only other thing this modem can
     * report here and it is indistinguishable from a fax at this layer. */
    record.mode = call.isVoice ? HRIL_CALL_VOICE : HRIL_CALL_DATA;
    record.mpty = call.isMpty ? 1 : 0;
    record.type = call.toa;
    /* CNAP presentation: 0 allowed, 1 restricted, 2 unknown, 3 payphone on
     * both sides. */
    record.namePresentation = call.namePresentation;
    return record;
}

HRilCallInfo ToCallInfo(CallRecord &record)
{
    HRilCallInfo info = {};
    info.index = record.index;
    info.dir = record.dir;
    info.state = static_cast<HRilCallState>(record.state);
    info.mode = static_cast<HRilCallMode>(record.mode);
    info.mpty = record.mpty;
    /* IRadioVoice is the CS domain by construction — 0 is "CS domain phone". */
    info.voiceDomain = 0;
    /* 0 is "voice call"; the video-call values are IMS-only. */
    info.callType = 0;
    info.number = const_cast<char *>(record.number.c_str());
    info.type = record.type;
    /*
     * `alpha` is the phonebook name matching the number and `name` the CNAP
     * name the network sent; AIDL only carries the latter.  V1_1 clients see
     * only `alpha`, V1_4 clients only use `name` (CSControl::
     * EncapsulationCallReportInfo), so the same string goes in both rather
     * than leaving the older field permanently empty.
     */
    info.alpha = const_cast<char *>(record.name.c_str());
    info.name = const_cast<char *>(record.name.c_str());
    info.namePresentation = record.namePresentation;
    return info;
}

/* ---- responses ------------------------------------------------------- */

class VoiceResponse : public rvoice::IRadioVoiceResponseDefault {
public:
    ::ndk::ScopedAStatus getCurrentCallsResponse(const radio::RadioResponseInfo &info,
                                                  const std::vector<rvoice::Call> &calls) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        std::vector<CallRecord> records;
        records.reserve(calls.size());
        for (const auto &call : calls) {
            records.push_back(ToRecord(call));
        }
        std::vector<HRilCallInfo> list;
        list.reserve(records.size());
        for (auto &record : records) {
            list.push_back(ToCallInfo(record));
        }
        for (const auto &record : records) {
            HR_LOGI("call %{public}d: state=%{public}d dir=%{public}d mpty=%{public}d",
                    record.index, record.state, record.dir, record.mpty);
        }
        HR_LOGI("call list: err=%{public}d %{public}zu call(s)",
                static_cast<int32_t>(info.error), list.size());
        /* GetCallListResponseExt accepts a null pointer as long as the length
         * is 0, and an empty list is the normal answer once the last call has
         * gone. */
        RilBridge::Get().ReportCall(req, ToHrilError(static_cast<int32_t>(info.error)),
                                    list.empty() ? nullptr : list.data(),
                                    list.size() * sizeof(HRilCallInfo));
        return ::ndk::ScopedAStatus::ok();
    }

    /*
     * The cause the network gave for the last call ending.  CS control asks
     * for this whenever a call disappears from the list without one, and only
     * reports the hang-up to call_manager once the answer arrives — so a
     * missing implementation does not merely lose the reason, it leaves the
     * call on screen (CSControl::HasEndCallWithoutReason).
     */
    ::ndk::ScopedAStatus getLastCallFailCauseResponse(
        const radio::RadioResponseInfo &info,
        const rvoice::LastCallFailCauseInfo &failCause) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        int32_t cause = static_cast<int32_t>(failCause.causeCode);
        HR_LOGI("last call fail cause: %{public}d (%{public}s)", cause,
                failCause.vendorCause.c_str());
        RilBridge::Get().ReportCall(req, ToHrilError(static_cast<int32_t>(info.error)), &cause,
                                    sizeof(cause));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getMuteResponse(const radio::RadioResponseInfo &info,
                                          bool enable) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        int32_t mute = enable ? 1 : 0;
        RilBridge::Get().ReportCall(req, ToHrilError(static_cast<int32_t>(info.error)), &mute,
                                    sizeof(mute));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus dialResponse(const radio::RadioResponseInfo &info) override
    {
        HR_LOGI("dial: err=%{public}d", static_cast<int32_t>(info.error));
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus acceptCallResponse(const radio::RadioResponseInfo &info) override
    {
        HR_LOGI("answer: err=%{public}d", static_cast<int32_t>(info.error));
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus rejectCallResponse(const radio::RadioResponseInfo &info) override
    {
        HR_LOGI("reject: err=%{public}d", static_cast<int32_t>(info.error));
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus hangupConnectionResponse(const radio::RadioResponseInfo &info) override
    {
        HR_LOGI("hangup: err=%{public}d", static_cast<int32_t>(info.error));
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus hangupWaitingOrBackgroundResponse(
        const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus hangupForegroundResumeBackgroundResponse(
        const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus switchWaitingOrHoldingAndActiveResponse(
        const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus conferenceResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus separateConnectionResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus explicitCallTransferResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus startDtmfResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus sendDtmfResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus stopDtmfResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus setMuteResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

private:
    ::ndk::ScopedAStatus ReportEmpty(const radio::RadioResponseInfo &info)
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        RilBridge::Get().ReportCall(req, ToHrilError(static_cast<int32_t>(info.error)), nullptr, 0);
        return ::ndk::ScopedAStatus::ok();
    }
};

/* ---- indications ----------------------------------------------------- */

class VoiceIndication : public rvoice::IRadioVoiceIndicationDefault {
public:
    explicit VoiceIndication(int32_t slotId) : slotId_(slotId) {}

    /*
     * The indication the whole domain turns on.  Neither side carries the new
     * state with it — it means "something about the calls changed, go and
     * look" — and OHOS answers it by issuing GetCallList
     * (CellularCallHandler::GetCsCallData).  Every incoming call, every state
     * transition and every hang-up reaches the framework through this one
     * notification.
     */
    ::ndk::ScopedAStatus callStateChanged(radio::RadioIndicationType type) override
    {
        HR_LOGI("call state changed (slot %{public}d)", slotId_);
        RilBridge::Get().NotifyCall(slotId_, HNOTI_CALL_STATE_UPDATED, nullptr, 0);
        AckIndication(type);
        return ::ndk::ScopedAStatus::ok();
    }

    /*
     * Whether the framework has to generate the ringback tone itself.
     * `start` true means the network is NOT sending in-band audio, which is
     * OHOS's RBTPlayInfo::LOCAL_ALERTING (1); false means it is, and the
     * audio path alone will carry it — NETWORK_ALERTING (0).
     */
    ::ndk::ScopedAStatus indicateRingbackTone(radio::RadioIndicationType type,
                                              bool start) override
    {
        int32_t status = start ? 1 : 0;
        HR_LOGI("ringback tone: %{public}s", start ? "local" : "network");
        RilBridge::Get().NotifyCall(slotId_, HNOTI_CALL_RINGBACK_VOICE_REPORT, &status,
                                    sizeof(status));
        AckIndication(type);
        return ::ndk::ScopedAStatus::ok();
    }

    /*
     * The emergency numbers this network accepts.  cellular_call compiles in
     * a 3GPP default list (112, 911, …) and merges what the modem reports on
     * top, so this only ever adds numbers — but the ones it adds are the
     * country-specific ones a traveller would actually dial.
     */
    ::ndk::ScopedAStatus currentEmergencyNumberList(
        radio::RadioIndicationType type,
        const std::vector<rvoice::EmergencyNumber> &numbers) override
    {
        struct EccRecord {
            std::string number;
            std::string mcc;
        };
        std::vector<EccRecord> records;
        records.reserve(numbers.size());
        for (const auto &number : numbers) {
            records.push_back({ number.number, number.mcc });
        }

        std::vector<HRilEmergencyInfo> list;
        list.reserve(numbers.size());
        for (size_t i = 0; i < numbers.size(); i++) {
            HRilEmergencyInfo info = {};
            info.index = static_cast<int32_t>(i);
            info.total = static_cast<int32_t>(numbers.size());
            info.eccNum = const_cast<char *>(records[i].number.c_str());
            /* EmergencyServiceCategory and OHOS's category field are the same
             * 3GPP TS 22.101 bitmap: 1 police, 2 ambulance, 4 fire,
             * 8 marine guard, 16 mountain rescue. */
            info.category = numbers[i].categories;
            /*
             * OHOS asks whether the number is valid with a card (1) or
             * without one (0).  AIDL instead says where the number came from,
             * so read it that way round: anything the network or the SIM told
             * us about is card-specific, and what the modem carries in its own
             * configuration is the no-card fallback.
             */
            constexpr int32_t SOURCE_NETWORK_SIGNALING = 1 << 0;
            constexpr int32_t SOURCE_SIM = 1 << 1;
            info.simpresent =
                (numbers[i].sources & (SOURCE_NETWORK_SIGNALING | SOURCE_SIM)) != 0 ? 1 : 0;
            info.mcc = const_cast<char *>(records[i].mcc.c_str());
            /* 0 = valid in every CS service state.  AIDL has no counterpart;
             * the restricted-service variant is a Huawei-modem notion. */
            info.abnormalService = 0;
            list.push_back(info);
        }

        HR_LOGI("emergency numbers (slot %{public}d): %{public}zu", slotId_, list.size());
        /* CallEmergencyNotice rejects an empty report outright, so drop it
         * here rather than have hril log a parameter error. */
        if (!list.empty()) {
            RilBridge::Get().NotifyCall(slotId_, HNOTI_CALL_EMERGENCY_NUMBER_REPORT, list.data(),
                                        list.size() * sizeof(HRilEmergencyInfo));
        }
        AckIndication(type);
        return ::ndk::ScopedAStatus::ok();
    }

    /*
     * Logged and dropped.  callRing duplicates callStateChanged for an
     * incoming call and OHOS has no notification for it; resendIncallMute
     * asks the client to re-send its mute setting, which OHOS drives from
     * call_manager on every answer anyway.
     */
    ::ndk::ScopedAStatus callRing(radio::RadioIndicationType type, bool isGsm,
                                  const rvoice::CdmaSignalInfoRecord &record) override
    {
        (void)isGsm;
        (void)record;
        HR_LOGD("call ring (slot %{public}d)", slotId_);
        AckIndication(type);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus resendIncallMute(radio::RadioIndicationType type) override
    {
        HR_LOGI("modem asked for the mute setting again (slot %{public}d)", slotId_);
        AckIndication(type);
        return ::ndk::ScopedAStatus::ok();
    }

private:
    void AckIndication(radio::RadioIndicationType type)
    {
        if (type != radio::RadioIndicationType::UNSOLICITED_ACK_EXP) {
            return;
        }
        auto voice = RilBridge::Get().Voice(slotId_);
        if (voice != nullptr) {
            voice->responseAcknowledgement();
        }
    }

    int32_t slotId_;
};

/* ---- HRilCallReq ----------------------------------------------------- */

void GetCallList(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    voice->getCurrentCalls(RilBridge::Get().Track(requestInfo));
}

void Dial(const ReqDataInfo *requestInfo, const HRilDial *data, size_t dataLen)
{
    (void)dataLen;
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    if (data == nullptr || data->address == nullptr) {
        HR_LOGE("no number to dial");
        RilBridge::Get().ReportCall(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    rvoice::Dial dial;
    dial.address = data->address;
    /* CLIR: 0 subscription default, 1 invocation, 2 suppression — the same
     * numbering as Dial::CLIR_* (at_call.h CallClirType). */
    dial.clir = data->clir;
    HR_LOGI("dial clir=%{public}d", dial.clir);
    voice->dial(RilBridge::Get().Track(requestInfo), dial);
}

void Answer(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    voice->acceptCall(RilBridge::Get().Track(requestInfo));
}

/*
 * Refuse an incoming call.  rejectCall is RIL_REQUEST_UDUB — "user determined
 * user busy" — which is what ATH does to a call that has not been answered,
 * and is not the same as hanging one up.
 */
void Reject(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    voice->rejectCall(RilBridge::Get().Track(requestInfo));
}

void Hangup(const ReqDataInfo *requestInfo, const uint32_t *data, size_t dataLen)
{
    (void)dataLen;
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    if (data == nullptr) {
        HR_LOGE("no call index");
        RilBridge::Get().ReportCall(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    int32_t index = static_cast<int32_t>(*data);
    HR_LOGI("hangup call %{public}d", index);
    voice->hangup(RilBridge::Get().Track(requestInfo), index);
}

/*
 * Hold, unhold and switch are one modem operation.  AT+CHLD=2 swaps the
 * active call with the held or waiting one, so which of the three names OHOS
 * uses only says what the caller believed the state was — at_call.c routes
 * all three to the same command for exactly this reason.
 */
void SwapCalls(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    voice->switchWaitingOrHoldingAndActive(RilBridge::Get().Track(requestInfo));
}

void CombineConference(const ReqDataInfo *requestInfo, int32_t callType)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    /* callType distinguishes the video modes of an IMS conference; a CS
     * conference is voice-only, so it carries no information here. */
    (void)callType;
    voice->conference(RilBridge::Get().Track(requestInfo));
}

void SeparateConference(const ReqDataInfo *requestInfo, int32_t callIndex, int32_t callType)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    (void)callType;
    if (callIndex <= 0) {
        HR_LOGE("bad call index %{public}d", callIndex);
        RilBridge::Get().ReportCall(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    voice->separateConnection(RilBridge::Get().Track(requestInfo), callIndex);
}

/*
 * The two multi-call hang-ups OHOS spells as one op with a type.  The values
 * are at_call.c's TYPE_HANG_UP_HOLD_WAIT and TYPE_HANG_UP_ACTIVE; the other
 * two documented in the header (release-all, and the release-held-and-waiting
 * variant) have no AT command behind them in that sample either.
 */
constexpr int32_t TYPE_HANG_UP_HOLD_WAIT = 0;
constexpr int32_t TYPE_HANG_UP_ACTIVE = 1;

void CallSupplement(const ReqDataInfo *requestInfo, int32_t type)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    switch (type) {
        case TYPE_HANG_UP_HOLD_WAIT:
            voice->hangupWaitingOrBackground(RilBridge::Get().Track(requestInfo));
            return;
        case TYPE_HANG_UP_ACTIVE:
            voice->hangupForegroundResumeBackground(RilBridge::Get().Track(requestInfo));
            return;
        default:
            HR_LOGE("unsupported call supplement type %{public}d", type);
            RilBridge::Get().ReportCall(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
            return;
    }
}

void ExplicitCallTransferConnection(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    voice->explicitCallTransfer(RilBridge::Get().Track(requestInfo));
}

/*
 * DTMF.  StartDtmf/StopDtmf are the press-and-hold pair (the tone lasts as
 * long as the key is down) and SendDtmf is one fixed-length burst; AIDL has
 * the same three.  OHOS's CallDtmfInfo carries a string, but every caller in
 * this tree sends a single character and AIDL's sendDtmf/startDtmf are
 * defined as one character, so anything longer is passed through and left to
 * the modem — which takes the first.
 */
void StartDtmf(const ReqDataInfo *requestInfo, CallDtmfInfo info)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    if (info.dtmfKey == nullptr) {
        HR_LOGE("no dtmf key");
        RilBridge::Get().ReportCall(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    voice->startDtmf(RilBridge::Get().Track(requestInfo), std::string(info.dtmfKey));
}

void SendDtmf(const ReqDataInfo *requestInfo, CallDtmfInfo info)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    if (info.dtmfKey == nullptr) {
        HR_LOGE("no dtmf key");
        RilBridge::Get().ReportCall(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    voice->sendDtmf(RilBridge::Get().Track(requestInfo), std::string(info.dtmfKey));
}

void StopDtmf(const ReqDataInfo *requestInfo, CallDtmfInfo info)
{
    (void)info;
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    voice->stopDtmf(RilBridge::Get().Track(requestInfo));
}

/*
 * Uplink mute.  cellular_call sends this on every answer
 * (CellularCallConnectionCS::AnswerRequest unmutes before ATA), so it is on
 * the path of a normal call even though the user never asks for it.
 */
void SetMute(const ReqDataInfo *requestInfo, int32_t mute)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    HR_LOGI("mute: %{public}d", mute);
    voice->setMute(RilBridge::Get().Track(requestInfo), mute != 0);
}

void GetMute(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    voice->getMute(RilBridge::Get().Track(requestInfo));
}

void GetCallFailReason(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::voice::IRadioVoice> voice;
    if (!RilBridge::Get().RequireVoice(requestInfo, &voice)) {
        return;
    }
    voice->getLastCallFailCause(RilBridge::Get().Track(requestInfo));
}

/*
 * Left NULL on purpose:
 *
 *   GetClip / SetClip / GetClir / SetClir / GetCallWaiting / SetCallWaiting /
 *   GetCallTransferInfo / SetCallTransferInfo / GetCallRestriction /
 *   SetCallRestriction / SetBarringPassword / SetUssd / GetUssd /
 *   CloseUnFinishedUssd / SetSuppSrvNotification
 *     Supplementary services — the settings you configure about calling
 *     rather than anything a call needs.  They arrive as one group: OHOS
 *     drives them from com.ohos.callsetting and from MMI codes typed into the
 *     dialler, both of which route through cellular_call's supplement
 *     requests, and none of them is reachable while a call is being set up.
 *     Most map cleanly (setClir, setCallWaiting, setCallForward, sendUssd),
 *     but two do not — SetClip has no AIDL counterpart at all, and call
 *     barring lives on IRadioSim as setFacilityLockForApp, not on IRadioVoice
 *     — so shipping half the group would leave the dialler's MMI handling
 *     failing in ways that look like carrier problems.  Plan phase R7.
 *
 *   GetEmergencyCallList / SetEmergencyCallList
 *     No AIDL counterpart in either direction: Android's modem announces its
 *     emergency numbers through currentEmergencyNumberList (which we do
 *     forward) and is never told them.  cellular_call treats both as
 *     best-effort — CellularCallService::RegisterCoreServiceHandler logs a
 *     warning and carries on — and compiles in the 3GPP list either way
 *     (CellularCallConfig::InitModeActive).
 *
 *   GetCallPreferenceMode / SetCallPreferenceMode
 *     The CS/IMS domain preference, an MTK vendor request with nothing behind
 *     it in AIDL.  cellular_call defaults to IMS_PS_VOICE_PREFERRED and only
 *     warns when the query fails.
 *
 *   GetImsCallList / GetLteImsSwitchStatus / SetLteImsSwitchStatus /
 *   SetVonrSwitch
 *     IMS, which is IRadioIms — plan phase R8.  (setVoNrEnabled does live on
 *     IRadioVoice, but switching VoNR on with no IMS stack underneath would
 *     move voice onto a domain nothing here can carry.)
 *
 *   GetTTYMode / SetTTYMode
 *     Teletypewriter mode for the hearing-impaired, which needs a TTY device
 *     on the headset jack to mean anything.  Nothing in this tree sets it.
 *
 * Not implemented on the indication side, for the same reasons:
 * onSupplementaryServiceIndication and onUssd (the supplementary-services
 * group), stkCallSetup and stkCallControlAlphaNotify (SIM toolkit), the cdma*
 * family (no CDMA network), and srvccStateNotify — which needs IMS, and whose
 * mapping carries a trap worth writing down for whoever does R8: AIDL's
 * SrvccState is STARTED/COMPLETED/FAILED/CANCELED (1..4) while OHOS's
 * HRilCallSrvccStatus documents 1..4 as starts/successful/cancelled/failed,
 * so the last two are transposed and cannot be passed through as integers.
 */
const HRilCallReq g_callOps = {
    .GetCallList = GetCallList,
    .Dial = Dial,
    .Hangup = Hangup,
    .Reject = Reject,
    .Answer = Answer,
    .GetClip = nullptr,
    .SetClip = nullptr,
    .HoldCall = SwapCalls,
    .UnHoldCall = SwapCalls,
    .SwitchCall = SwapCalls,
    .CombineConference = CombineConference,
    .SeparateConference = SeparateConference,
    .CallSupplement = CallSupplement,
    .GetCallWaiting = nullptr,
    .SetCallWaiting = nullptr,
    .SetCallTransferInfo = nullptr,
    .GetCallTransferInfo = nullptr,
    .GetCallRestriction = nullptr,
    .SetCallRestriction = nullptr,
    .GetClir = nullptr,
    .SetClir = nullptr,
    .StartDtmf = StartDtmf,
    .SendDtmf = SendDtmf,
    .StopDtmf = StopDtmf,
    .GetImsCallList = nullptr,
    .GetCallPreferenceMode = nullptr,
    .SetCallPreferenceMode = nullptr,
    .GetLteImsSwitchStatus = nullptr,
    .SetLteImsSwitchStatus = nullptr,
    .SetUssd = nullptr,
    .GetUssd = nullptr,
    .SetMute = SetMute,
    .GetMute = GetMute,
    .GetEmergencyCallList = nullptr,
    .GetCallFailReason = GetCallFailReason,
    .SetEmergencyCallList = nullptr,
    .SetBarringPassword = nullptr,
    .CloseUnFinishedUssd = nullptr,
    .ExplicitCallTransferConnection = ExplicitCallTransferConnection,
    .SetVonrSwitch = nullptr,
    .SetSuppSrvNotification = nullptr,
    .GetTTYMode = nullptr,
    .SetTTYMode = nullptr,
};

} // namespace

const HRilCallReq *CallOps()
{
    return &g_callOps;
}

void AttachVoiceCallbacks(int32_t slotId, const std::shared_ptr<radio::voice::IRadioVoice> &voice)
{
    static std::shared_ptr<rvoice::IRadioVoiceResponseDelegator> resp[MAX_SLOTS];
    static std::shared_ptr<rvoice::IRadioVoiceIndicationDelegator> ind[MAX_SLOTS];

    resp[slotId] = ::ndk::SharedRefBase::make<rvoice::IRadioVoiceResponseDelegator>(
        ::ndk::SharedRefBase::make<VoiceResponse>());
    ind[slotId] = ::ndk::SharedRefBase::make<rvoice::IRadioVoiceIndicationDelegator>(
        ::ndk::SharedRefBase::make<VoiceIndication>(slotId));

    auto st = voice->setResponseFunctions(resp[slotId], ind[slotId]);
    if (!st.isOk()) {
        HR_LOGE("voice setResponseFunctions: %{public}s", st.getDescription().c_str());
    }
}

} // namespace HybrisRil
} // namespace OHOS
