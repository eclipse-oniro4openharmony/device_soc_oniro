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
 * Presence registration for the radio interfaces we do not implement yet.
 *
 * MTK's rild does not treat each IRadio interface independently.  Every
 * AOSP setResponseFunctions() lands in librilfusion.so, which does:
 *
 *     rilAidlUtils::setAospResponseNumberToOne(type, slot);
 *     if (rilAidlUtils::checkIfSetAllAospResponseDone(slot))
 *         android::onNewCommandConnect(slot, ...);
 *
 * checkIfSetAllAospResponseDone() is an AND over *seven* per-slot type
 * slots (data, messaging, modem, network, sim, voice, ims), and only when
 * all seven have registered does onNewCommandConnect() fire.  That is what
 * moves the RFX connection state, which RtcGsmSmsController watches
 * (registerStatusChanged key 116 -> onHidlStateChanged), and which sets
 * RFX_STATUS_KEY_SMS_FW_READY.
 *
 * Until that flag is true, RtcGsmSmsController::onPreviewMessage refuses to
 * deliver an inbound SMS: it logs "SMS framework isn't ready yet. queue
 * RFX_MSG_URC_RESPONSE_NEW_SMS", starts a 10 s timer, and on expiry NACKs
 * the network — which then retries the message and is refused again.  A
 * client that registers on only the interfaces it needs therefore gets a
 * working modem, SIM and network but silently never receives an SMS, with
 * nothing wrong on the OHOS side to find.
 *
 * So we register on all seven.  Three of them have no implementation here
 * (data is R5, voice R6, ims R8); they get the generated Default handlers,
 * which answer every callback with STATUS_UNKNOWN_TRANSACTION.  rild's
 * calls into them are oneway, so nothing observes that, and the
 * indications they would carry are ones we do not act on yet.
 *
 * When R5 and R6 land they should replace their stub here with a real
 * response/indication object rather than adding a second registration —
 * setResponseFunctions overwrites, so registering twice would drop
 * whichever came first.
 */

#include <memory>
#include <string>

#include <aidl/android/hardware/radio/data/BnRadioDataIndication.h>
#include <aidl/android/hardware/radio/data/BnRadioDataResponse.h>
#include <aidl/android/hardware/radio/data/IRadioData.h>
#include <aidl/android/hardware/radio/ims/BnRadioImsIndication.h>
#include <aidl/android/hardware/radio/ims/BnRadioImsResponse.h>
#include <aidl/android/hardware/radio/ims/IRadioIms.h>
#include <aidl/android/hardware/radio/voice/BnRadioVoiceIndication.h>
#include <aidl/android/hardware/radio/voice/BnRadioVoiceResponse.h>
#include <aidl/android/hardware/radio/voice/IRadioVoice.h>

#include "hybris_ril_bridge.h"
#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {
namespace {

namespace rdata = aidl::android::hardware::radio::data;
namespace rvoice = aidl::android::hardware::radio::voice;
namespace rims = aidl::android::hardware::radio::ims;

/*
 * One registration.  Kept as a template because the three are identical
 * apart from their types: look the service up, wrap the generated Default
 * in the generated Delegator (a Bn* skeleton, so it is a real binder
 * object), and hand it over.
 */
template <typename Iface, typename RespDefault, typename RespDelegator,
          typename IndDefault, typename IndDelegator>
bool RegisterStub(int32_t slotId, const char *what,
                  std::shared_ptr<RespDelegator> *respSlot,
                  std::shared_ptr<IndDelegator> *indSlot)
{
    const std::string name =
        std::string(Iface::descriptor) + "/slot" + std::to_string(slotId + 1);

    auto service = Iface::fromBinder(RilBridge::Get().AwaitService(name));
    if (service == nullptr) {
        HR_LOGE("no %{public}s — MT SMS will not be delivered", name.c_str());
        return false;
    }

    *respSlot = ::ndk::SharedRefBase::make<RespDelegator>(
        ::ndk::SharedRefBase::make<RespDefault>());
    *indSlot = ::ndk::SharedRefBase::make<IndDelegator>(
        ::ndk::SharedRefBase::make<IndDefault>());

    auto st = service->setResponseFunctions(*respSlot, *indSlot);
    if (!st.isOk()) {
        HR_LOGE("%{public}s setResponseFunctions: %{public}s", what,
                st.getDescription().c_str());
        return false;
    }
    HR_LOGI("%{public}s registered (stub)", what);
    return true;
}

} // namespace

bool AttachPresenceCallbacks(int32_t slotId)
{
    static std::shared_ptr<rdata::IRadioDataResponseDelegator> dataResp[MAX_SLOTS];
    static std::shared_ptr<rdata::IRadioDataIndicationDelegator> dataInd[MAX_SLOTS];
    static std::shared_ptr<rvoice::IRadioVoiceResponseDelegator> voiceResp[MAX_SLOTS];
    static std::shared_ptr<rvoice::IRadioVoiceIndicationDelegator> voiceInd[MAX_SLOTS];
    static std::shared_ptr<rims::IRadioImsResponseDelegator> imsResp[MAX_SLOTS];
    static std::shared_ptr<rims::IRadioImsIndicationDelegator> imsInd[MAX_SLOTS];

    bool ok = true;

    ok &= RegisterStub<rdata::IRadioData, rdata::IRadioDataResponseDefault,
                       rdata::IRadioDataResponseDelegator,
                       rdata::IRadioDataIndicationDefault,
                       rdata::IRadioDataIndicationDelegator>(
        slotId, "data", &dataResp[slotId], &dataInd[slotId]);

    ok &= RegisterStub<rvoice::IRadioVoice, rvoice::IRadioVoiceResponseDefault,
                       rvoice::IRadioVoiceResponseDelegator,
                       rvoice::IRadioVoiceIndicationDefault,
                       rvoice::IRadioVoiceIndicationDelegator>(
        slotId, "voice", &voiceResp[slotId], &voiceInd[slotId]);

    ok &= RegisterStub<rims::IRadioIms, rims::IRadioImsResponseDefault,
                       rims::IRadioImsResponseDelegator,
                       rims::IRadioImsIndicationDefault,
                       rims::IRadioImsIndicationDelegator>(
        slotId, "ims", &imsResp[slotId], &imsInd[slotId]);

    return ok;
}

} // namespace HybrisRil
} // namespace OHOS
