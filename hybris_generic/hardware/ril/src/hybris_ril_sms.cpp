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
 * smsOps — HRilSmsReq mapped onto android.hardware.radio.messaging v2
 * (plan §5 R4).
 *
 * The two sides agree on the wire format, which makes this the least lossy
 * domain so far: OHOS hands us `smscPdu`/`pdu` already hex-encoded
 * (SmsSender does StringUtils::StringToHex before it reaches hril) and
 * GsmSmsMessage wants exactly that, so MO messages pass straight through.
 *
 * MT is the one place a conversion is needed, and it goes the other way:
 * the AIDL indications carry raw PDU *bytes* while hril re-parses what we
 * hand it with ConvertHexStringToBytes.  Handing over the bytes unchanged
 * silently yields a half-length garbage PDU rather than an error, so
 * everything inbound goes through BytesToHex().
 *
 * The two notifications also disagree with each other about their payload,
 * which is not a mistake on our side — see NewSms/StatusReport below.
 */

#include <memory>
#include <string>
#include <vector>

#include <aidl/android/hardware/radio/messaging/BnRadioMessagingIndication.h>
#include <aidl/android/hardware/radio/messaging/BnRadioMessagingResponse.h>
#include <aidl/android/hardware/radio/messaging/GsmSmsMessage.h>
#include <aidl/android/hardware/radio/messaging/SendSmsResult.h>
#include <aidl/android/hardware/radio/messaging/SmsAcknowledgeFailCause.h>
#include <aidl/android/hardware/radio/messaging/SmsWriteArgs.h>

#include "hril_notification.h"
#include "hril_request.h"
#include "hybris_ril_bridge.h"
#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {
namespace {

namespace rmsg = aidl::android::hardware::radio::messaging;

/* hril parses every PDU it is given with ConvertHexStringToBytes, so the
 * bytes an AIDL indication carries have to be re-encoded before they are
 * handed over.  Upper case: that is what the AT vendor emits and what the
 * framework's own StringToHex produces. */
std::string BytesToHex(const std::vector<uint8_t> &bytes)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

/*
 * SMSC address type-of-address, 3GPP TS 24.011.  IRadioMessaging deals in a
 * bare string where OHOS wants the +CSCA pair, so the leading '+' carries
 * the information both ways: 145 = international, 129 = unknown/national.
 */
constexpr int32_t TOSCA_INTERNATIONAL = 145;
constexpr int32_t TOSCA_UNKNOWN = 129;

int32_t ToscaOf(const std::string &address)
{
    return (!address.empty() && address[0] == '+') ? TOSCA_INTERNATIONAL : TOSCA_UNKNOWN;
}

std::string ApplyTosca(int32_t tosca, const char *address)
{
    std::string value = (address != nullptr) ? address : "";
    if (tosca == TOSCA_INTERNATIONAL && !value.empty() && value[0] != '+') {
        value.insert(value.begin(), '+');
    }
    return value;
}

/*
 * AckIncomeCause (sms_mms/services/sms/include/sms_common.h) onto the two
 * causes IRadioMessaging v2 defines.  The value only matters when the ack is
 * a failure; "out of memory" is the one the network is expected to act on by
 * holding the message for redelivery.
 */
constexpr int32_t SMS_ACK_OUT_OF_MEMORY = 0x03;

rmsg::SmsAcknowledgeFailCause ToAckFailCause(int32_t ackIncomeCause)
{
    return (ackIncomeCause == SMS_ACK_OUT_OF_MEMORY)
               ? rmsg::SmsAcknowledgeFailCause::MEMORY_CAPACITY_EXCEEDED
               : rmsg::SmsAcknowledgeFailCause::UNSPECIFIED_ERROR;
}

/* Every IRadio method is oneway, so acknowledging an indication from inside
 * the indication callback cannot deadlock. */
void AckIndication(int32_t slotId, radio::RadioIndicationType type)
{
    if (type != radio::RadioIndicationType::UNSOLICITED_ACK_EXP) {
        return;
    }
    auto messaging = RilBridge::Get().Messaging(slotId);
    if (messaging != nullptr) {
        messaging->responseAcknowledgement();
    }
}

/* ---- responses ------------------------------------------------------- */

class MessagingResponse : public rmsg::IRadioMessagingResponseDefault {
public:
    ::ndk::ScopedAStatus sendSmsResponse(const radio::RadioResponseInfo &info,
                                         const rmsg::SendSmsResult &result) override
    {
        return ReportSendResult(info, result);
    }

    ::ndk::ScopedAStatus sendSmsExpectMoreResponse(const radio::RadioResponseInfo &info,
                                                   const rmsg::SendSmsResult &result) override
    {
        return ReportSendResult(info, result);
    }

    ::ndk::ScopedAStatus acknowledgeLastIncomingGsmSmsResponse(
        const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus getSmscAddressResponse(const radio::RadioResponseInfo &info,
                                                const std::string &smsc) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HRilServiceCenterAddress address = {};
        address.tosca = ToscaOf(smsc);
        address.address = const_cast<char *>(smsc.c_str());
        HR_LOGI("SMSC: err=%{public}d tosca=%{public}d len=%{public}zu",
                static_cast<int32_t>(info.error), address.tosca, smsc.size());
        RilBridge::Get().ReportSms(req, ToHrilError(static_cast<int32_t>(info.error)), &address,
                                   sizeof(address));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus setSmscAddressResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus reportSmsMemoryStatusResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

    /* hril's AddSimMessageResponse carries no payload, so the record index
     * rild returns here is dropped — the framework re-reads the store. */
    ::ndk::ScopedAStatus writeSmsToSimResponse(const radio::RadioResponseInfo &info,
                                               int32_t /*index*/) override
    {
        return ReportEmpty(info);
    }

    ::ndk::ScopedAStatus deleteSmsOnSimResponse(const radio::RadioResponseInfo &info) override
    {
        return ReportEmpty(info);
    }

private:
    ::ndk::ScopedAStatus ReportSendResult(const radio::RadioResponseInfo &info,
                                          const rmsg::SendSmsResult &result)
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HRilSmsResponse response = {};
        response.msgRef = result.messageRef;
        response.pdu = const_cast<char *>(result.ackPDU.c_str());
        response.errCode = result.errorCode;
        HR_LOGI("sendSms: err=%{public}d msgRef=%{public}d errCode=%{public}d",
                static_cast<int32_t>(info.error), result.messageRef, result.errorCode);
        RilBridge::Get().ReportSms(req, ToHrilError(static_cast<int32_t>(info.error)), &response,
                                   sizeof(response));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus ReportEmpty(const radio::RadioResponseInfo &info)
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        RilBridge::Get().ReportSms(req, ToHrilError(static_cast<int32_t>(info.error)), nullptr, 0);
        return ::ndk::ScopedAStatus::ok();
    }
};

/* ---- indications ----------------------------------------------------- */

class MessagingIndication : public rmsg::IRadioMessagingIndicationDefault {
public:
    explicit MessagingIndication(int32_t slotId) : slotId_(slotId) {}

    /*
     * HNOTI_SMS_NEW_SMS takes an HRilSmsResponse whose `pdu` points at a hex
     * string, and HRilSms::NewSmsNotify rejects anything shorter than the
     * struct — so the length is sizeof(), not the PDU length.  (The AT
     * vendor passes strlen(pdu) here and only gets away with it because a
     * real PDU is always longer than the struct.)
     */
    ::ndk::ScopedAStatus newSms(radio::RadioIndicationType type,
                                const std::vector<uint8_t> &pdu) override
    {
        std::string hex = BytesToHex(pdu);
        HR_LOGI("new SMS (slot %{public}d, %{public}zu bytes)", slotId_, pdu.size());
        HRilSmsResponse response = {};
        response.pdu = const_cast<char *>(hex.c_str());
        RilBridge::Get().NotifySms(slotId_, HNOTI_SMS_NEW_SMS, &response, sizeof(response));
        AckIndication(slotId_, type);
        return ::ndk::ScopedAStatus::ok();
    }

    /*
     * ...whereas HNOTI_SMS_STATUS_REPORT takes the hex string *itself* as
     * the payload, with the string length as the length.  The two
     * notifications genuinely disagree; HRilSms::SmsStatusReportNotify
     * hex-decodes `response` directly.
     */
    ::ndk::ScopedAStatus newSmsStatusReport(radio::RadioIndicationType type,
                                            const std::vector<uint8_t> &pdu) override
    {
        std::string hex = BytesToHex(pdu);
        HR_LOGI("SMS status report (slot %{public}d)", slotId_);
        RilBridge::Get().NotifySms(slotId_, HNOTI_SMS_STATUS_REPORT, hex.c_str(), hex.size());
        AckIndication(slotId_, type);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus newSmsOnSim(radio::RadioIndicationType type,
                                     int32_t recordNumber) override
    {
        RilBridge::Get().NotifySms(slotId_, HNOTI_SMS_NEW_SMS_STORED_ON_SIM, &recordNumber,
                                   sizeof(recordNumber));
        AckIndication(slotId_, type);
        return ::ndk::ScopedAStatus::ok();
    }

    /* HNOTI_SMS_SIM_SMS_MEMORY_FULL exists but HRilSms registers no handler
     * for it, so raising it would only be dropped a layer further on. */
    ::ndk::ScopedAStatus simSmsStorageFull(radio::RadioIndicationType type) override
    {
        HR_LOGW("SIM SMS storage full (slot %{public}d)", slotId_);
        AckIndication(slotId_, type);
        return ::ndk::ScopedAStatus::ok();
    }

private:
    int32_t slotId_;
};

/* ---- HRilSmsReq ------------------------------------------------------ */

/*
 * Serves both HREQ_SMS_SEND_GSM_SMS and HREQ_SMS_SEND_SMS_MORE_MODE: hril
 * routes them through the same vendor slot (HRilSms::SendSmsMoreMode calls
 * RequestWithStrings with &HRilSmsReq::SendGsmSms), so the request code is
 * the only thing that distinguishes them.  It matters — "more mode" is what
 * keeps the link up between the segments of a concatenated message.
 *
 * `dataLen` is a count of strings, not a byte count: data[0] is the SMSC
 * PDU, data[1] the message PDU, both already hex.
 */
void SendGsmSms(const ReqDataInfo *requestInfo, const char *const *data, size_t dataLen)
{
    std::shared_ptr<radio::messaging::IRadioMessaging> messaging;
    if (!RilBridge::Get().RequireSms(requestInfo, &messaging)) {
        return;
    }
    constexpr size_t SMS_STRING_COUNT = 2;
    if (data == nullptr || dataLen < SMS_STRING_COUNT) {
        HR_LOGE("SendGsmSms: %{public}zu strings", dataLen);
        RilBridge::Get().ReportSms(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }

    rmsg::GsmSmsMessage message;
    message.smscPdu = (data[0] != nullptr) ? data[0] : "";
    message.pdu = (data[1] != nullptr) ? data[1] : "";

    bool expectMore = (requestInfo->request == HREQ_SMS_SEND_SMS_MORE_MODE);
    int32_t serial = RilBridge::Get().Track(requestInfo);
    HR_LOGI("send SMS: %{public}s smsc=%{public}zu pdu=%{public}zu serial=%{public}d",
            expectMore ? "expectMore" : "single", message.smscPdu.size(), message.pdu.size(),
            serial);
    if (expectMore) {
        messaging->sendSmsExpectMore(serial, message);
    } else {
        messaging->sendSms(serial, message);
    }
}

/*
 * data[0] is the result (0/1), data[1] the AckIncomeCause.  `dataLen` is
 * sizeof(int32_t) regardless of how many ints are there — HRilSms::
 * SendSmsAck hardcodes it — so it cannot be used to validate the buffer;
 * the allocation is always the two ints RequestWithInts was given.
 */
void SendSmsAck(const ReqDataInfo *requestInfo, const int32_t *data, size_t dataLen)
{
    std::shared_ptr<radio::messaging::IRadioMessaging> messaging;
    if (!RilBridge::Get().RequireSms(requestInfo, &messaging)) {
        return;
    }
    if (data == nullptr || dataLen == 0) {
        HR_LOGE("SendSmsAck: no data");
        RilBridge::Get().ReportSms(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    bool success = (data[0] != 0);
    HR_LOGI("ack incoming SMS: success=%{public}d cause=%{public}d", success, data[1]);
    messaging->acknowledgeLastIncomingGsmSms(RilBridge::Get().Track(requestInfo), success,
                                             ToAckFailCause(data[1]));
}

void GetSmscAddr(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::messaging::IRadioMessaging> messaging;
    if (!RilBridge::Get().RequireSms(requestInfo, &messaging)) {
        return;
    }
    HR_LOGI("get SMSC");
    messaging->getSmscAddress(RilBridge::Get().Track(requestInfo));
}

void SetSmscAddr(const ReqDataInfo *requestInfo, const HRilServiceCenterAddress *data,
                 size_t dataLen)
{
    std::shared_ptr<radio::messaging::IRadioMessaging> messaging;
    if (!RilBridge::Get().RequireSms(requestInfo, &messaging)) {
        return;
    }
    if (data == nullptr || dataLen < sizeof(HRilServiceCenterAddress)) {
        HR_LOGE("SetSmscAddr: bad payload");
        RilBridge::Get().ReportSms(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    messaging->setSmscAddress(RilBridge::Get().Track(requestInfo),
                              ApplyTosca(data->tosca, data->address));
}

void SetSmsMemoryStatus(const ReqDataInfo *requestInfo, int32_t available)
{
    std::shared_ptr<radio::messaging::IRadioMessaging> messaging;
    if (!RilBridge::Get().RequireSms(requestInfo, &messaging)) {
        return;
    }
    messaging->reportSmsMemoryStatus(RilBridge::Get().Track(requestInfo), available != 0);
}

void AddSimMessage(const ReqDataInfo *requestInfo, const HRilSmsWriteSms *data, size_t dataLen)
{
    std::shared_ptr<radio::messaging::IRadioMessaging> messaging;
    if (!RilBridge::Get().RequireSms(requestInfo, &messaging)) {
        return;
    }
    if (data == nullptr || dataLen < sizeof(HRilSmsWriteSms)) {
        HR_LOGE("AddSimMessage: bad payload");
        RilBridge::Get().ReportSms(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    /* state is TS 27.005 3.1, the same numbering as SmsWriteArgs::STATUS_*. */
    rmsg::SmsWriteArgs args;
    args.status = data->state;
    args.pdu = (data->pdu != nullptr) ? data->pdu : "";
    args.smsc = (data->smsc != nullptr) ? data->smsc : "";
    HR_LOGI("write SMS to SIM: status=%{public}d pdu=%{public}zu", args.status, args.pdu.size());
    messaging->writeSmsToSim(RilBridge::Get().Track(requestInfo), args);
}

void DelSimMessage(const ReqDataInfo *requestInfo, const int32_t *data, size_t dataLen)
{
    std::shared_ptr<radio::messaging::IRadioMessaging> messaging;
    if (!RilBridge::Get().RequireSms(requestInfo, &messaging)) {
        return;
    }
    if (data == nullptr || dataLen == 0) {
        HR_LOGE("DelSimMessage: no index");
        RilBridge::Get().ReportSms(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }
    HR_LOGI("delete SMS on SIM: index=%{public}d", data[0]);
    messaging->deleteSmsOnSim(RilBridge::Get().Track(requestInfo), data[0]);
}

/*
 * UpdateSimMessage stays NULL deliberately.  IRadioMessaging has no update
 * verb — Android's framework implements it as deleteSmsOnSim followed by
 * writeSmsToSim — and chaining those here would mean inventing a
 * two-request state machine whose failure modes (delete succeeds, write
 * fails) lose the message.  VENDOR_NOT_IMPLEMENT is the honest answer until
 * something actually needs SIM-stored SMS editing.
 *
 * The cell-broadcast ops are NULL for the same reason as the CDMA ones:
 * untestable on this carrier, and a wrong HRilCBConfigInfo range mapping is
 * worse than an unimplemented one.
 */
const HRilSmsReq g_smsOps = {
    .SendGsmSms = SendGsmSms,
    .SendSmsAck = SendSmsAck,
    .SendCdmaSms = nullptr,
    .SendCdmaAck = nullptr,
    .AddSimMessage = AddSimMessage,
    .DelSimMessage = DelSimMessage,
    .UpdateSimMessage = nullptr,
    .SetSmscAddr = SetSmscAddr,
    .GetSmscAddr = GetSmscAddr,
    .SetCBConfig = nullptr,
    .GetCBConfig = nullptr,
    .GetCdmaCBConfig = nullptr,
    .SetCdmaCBConfig = nullptr,
    .AddCdmaSimMessage = nullptr,
    .AddCdmaSimMessageV2 = nullptr,
    .DelCdmaSimMessage = nullptr,
    .UpdateCdmaSimMessage = nullptr,
    .SetSmsMemoryStatus = SetSmsMemoryStatus,
};

} // namespace

const HRilSmsReq *SmsOps()
{
    return &g_smsOps;
}

void AttachMessagingCallbacks(int32_t slotId,
                              const std::shared_ptr<radio::messaging::IRadioMessaging> &messaging)
{
    static std::shared_ptr<rmsg::IRadioMessagingResponseDelegator> resp[MAX_SLOTS];
    static std::shared_ptr<rmsg::IRadioMessagingIndicationDelegator> ind[MAX_SLOTS];

    resp[slotId] = ::ndk::SharedRefBase::make<rmsg::IRadioMessagingResponseDelegator>(
        ::ndk::SharedRefBase::make<MessagingResponse>());
    ind[slotId] = ::ndk::SharedRefBase::make<rmsg::IRadioMessagingIndicationDelegator>(
        ::ndk::SharedRefBase::make<MessagingIndication>(slotId));

    auto st = messaging->setResponseFunctions(resp[slotId], ind[slotId]);
    if (!st.isOk()) {
        HR_LOGE("messaging setResponseFunctions: %{public}s", st.getDescription().c_str());
    }
}

} // namespace HybrisRil
} // namespace OHOS
