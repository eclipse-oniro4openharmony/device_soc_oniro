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

/* modemOps — HRilModemReq mapped onto android.hardware.radio.modem v2. */

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <aidl/android/hardware/radio/modem/BnRadioModemIndication.h>
#include <aidl/android/hardware/radio/modem/BnRadioModemResponse.h>

#include "hril_notification.h"
#include "hril_request.h"
#include "hybris_ril_bridge.h"
#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {
namespace {

/*
 * The modem's last known power state.  IRadio v2 has no getRadioState — the
 * value only ever arrives as a radioStateChanged indication, and rild sends
 * that on a *change*, so a client attaching to an already-powered modem is
 * told nothing.  Track it from the two events that do carry the truth: the
 * indication, and our own successful setRadioPower.
 *
 * Getting this wrong is not subtle.  core_service's NetworkSearchHandler
 * polls GetRadioState every 3 s and calls NetworkSearchState::SetInitial —
 * wiping the registration it just learned — for as long as the answer is
 * anything but ON.  The two wrong answers fail differently:
 *
 *   UNAVAILABLE  RadioOffOrUnavailableState just waits for the modem to
 *                appear, so nothing ever powers the radio on.
 *   OFF          the same state machine reads airplane mode, decides the
 *                radio should be on, and calls SetRadioState(1).
 *
 * So OFF is the right starting point: it makes the framework assert the
 * state it wants, and the setRadioPower response tells us the truth.
 */
std::atomic<int32_t> g_radioState{HRIL_RADIO_POWER_STATE_OFF};

/* setRadioPower's response does not echo the value that was requested, so
 * remember it per serial.  Small and short-lived: hril serialises requests. */
std::mutex g_powerLock;
std::map<int32_t, bool> g_powerRequests;

int32_t ToHrilRadioState(radio::modem::RadioState state)
{
    switch (state) {
        case radio::modem::RadioState::OFF: return HRIL_RADIO_POWER_STATE_OFF;
        case radio::modem::RadioState::ON:  return HRIL_RADIO_POWER_STATE_ON;
        default:                            return HRIL_RADIO_POWER_STATE_UNAVAILABLE;
    }
}

/* ---- responses ------------------------------------------------------- */

class ModemResponse : public radio::modem::IRadioModemResponseDefault {
public:
    ::ndk::ScopedAStatus getBasebandVersionResponse(const radio::RadioResponseInfo &info,
                                                    const std::string &version) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        /* hril reads string payloads as `std::string((const char *)response)`
         * — the buffer IS the NUL-terminated text, not a pointer to it. */
        RilBridge::Get().ReportModem(req, ToHrilError(static_cast<int32_t>(info.error)),
                                     version.c_str(), version.size());
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getImeiResponse(
        const radio::RadioResponseInfo &info,
        const std::optional<radio::modem::ImeiInfo> &imeiInfo) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        /* One IRadio call answers two HRil requests; the tracker remembers
         * which one asked. */
        const std::string value = imeiInfo.has_value()
            ? (req->request == HREQ_MODEM_GET_IMEISV ? imeiInfo->svn : imeiInfo->imei)
            : std::string();
        RilBridge::Get().ReportModem(req, ToHrilError(static_cast<int32_t>(info.error)),
                                     value.c_str(), value.size());
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus setRadioPowerResponse(const radio::RadioResponseInfo &info) override
    {
        bool requestedOn = false;
        bool known = false;
        {
            std::lock_guard<std::mutex> guard(g_powerLock);
            auto it = g_powerRequests.find(info.serial);
            if (it != g_powerRequests.end()) {
                requestedOn = it->second;
                known = true;
                g_powerRequests.erase(it);
            }
        }

        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (known && info.error == radio::RadioError::NONE) {
            int32_t state = requestedOn ? HRIL_RADIO_POWER_STATE_ON
                                        : HRIL_RADIO_POWER_STATE_OFF;
            if (g_radioState.exchange(state) != state) {
                HR_LOGI("radio power -> %{public}d", state);
                RilBridge::Get().NotifyModem(req != nullptr ? req->slotId : 0,
                                             HNOTI_MODEM_RADIO_STATE_UPDATED, &state,
                                             sizeof(state));
            }
        }
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        RilBridge::Get().ReportModem(req, ToHrilError(static_cast<int32_t>(info.error)),
                                     nullptr, 0);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus requestShutdownResponse(const radio::RadioResponseInfo &info) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        RilBridge::Get().ReportModem(req, ToHrilError(static_cast<int32_t>(info.error)),
                                     nullptr, 0);
        return ::ndk::ScopedAStatus::ok();
    }

    /* Anything we fired but do not decode still has to close its request, or
     * hril leaks the ReqDataInfo and the framework waits forever. */
    ::ndk::ScopedAStatus acknowledgeRequest(int32_t serial) override
    {
        (void)serial;
        return ::ndk::ScopedAStatus::ok();
    }
};

/* ---- indications ----------------------------------------------------- */

class ModemIndication : public radio::modem::IRadioModemIndicationDefault {
public:
    explicit ModemIndication(int32_t slotId) : slotId_(slotId) {}

    ::ndk::ScopedAStatus radioStateChanged(radio::RadioIndicationType,
                                           radio::modem::RadioState radioState) override
    {
        int32_t state = ToHrilRadioState(radioState);
        g_radioState.store(state);
        HR_LOGI("radio state -> %{public}d", state);
        RilBridge::Get().NotifyModem(slotId_, HNOTI_MODEM_RADIO_STATE_UPDATED, &state,
                                     sizeof(state));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus rilConnected(radio::RadioIndicationType) override
    {
        HR_LOGI("rild connected (slot %{public}d)", slotId_);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus modemReset(radio::RadioIndicationType, const std::string &reason) override
    {
        HR_LOGW("modem reset: %{public}s", reason.c_str());
        int32_t state = HRIL_RADIO_POWER_STATE_UNAVAILABLE;
        g_radioState.store(state);
        RilBridge::Get().NotifyModem(slotId_, HNOTI_MODEM_RADIO_STATE_UPDATED, &state,
                                     sizeof(state));
        return ::ndk::ScopedAStatus::ok();
    }

private:
    int32_t slotId_;
};

/* ---- HRilModemReq ---------------------------------------------------- */

void SetRadioState(const ReqDataInfo *requestInfo, int32_t fun, int32_t rst)
{
    (void)rst;
    std::shared_ptr<radio::modem::IRadioModem> modem;
    if (!RilBridge::Get().RequireModem(requestInfo, &modem)) {
        return;
    }
    bool on = (fun != HRIL_RADIO_POWER_STATE_OFF);
    int32_t serial = RilBridge::Get().Track(requestInfo);
    {
        std::lock_guard<std::mutex> guard(g_powerLock);
        g_powerRequests[serial] = on;
    }
    modem->setRadioPower(serial, on, false, false);
}

void GetRadioState(const ReqDataInfo *requestInfo)
{
    if (requestInfo == nullptr) {
        return;
    }
    /* Answered from the cached indication state — see g_radioState. */
    int32_t state = g_radioState.load();
    RilBridge::Get().ReportModem(requestInfo, HRIL_ERR_SUCCESS, &state, sizeof(state));
}

void GetImei(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::modem::IRadioModem> modem;
    if (!RilBridge::Get().RequireModem(requestInfo, &modem)) {
        return;
    }
    modem->getImei(RilBridge::Get().Track(requestInfo));
}

void GetImeiSv(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::modem::IRadioModem> modem;
    if (!RilBridge::Get().RequireModem(requestInfo, &modem)) {
        return;
    }
    modem->getImei(RilBridge::Get().Track(requestInfo));
}

void GetMeid(const ReqDataInfo *requestInfo)
{
    /* CDMA only; this modem is 3GPP.  Answer empty rather than not-implemented
     * so core_service's device-identity fetch completes. */
    if (requestInfo == nullptr) {
        return;
    }
    RilBridge::Get().ReportModem(requestInfo, HRIL_ERR_SUCCESS, "", 0);
}

void GetVoiceRadioTechnology(const ReqDataInfo *requestInfo)
{
    /* IRadio has no equivalent request — the RAT comes with the network
     * registration state (plan phase R3).  Until networkOps lands, report a
     * minimal record so core_service's VoiceRadioTechnology query completes
     * instead of timing out. */
    if (requestInfo == nullptr) {
        return;
    }
    static char emptyName[] = "";
    HRilVoiceRadioInfo info = {};
    info.sysModeName = emptyName;
    info.actType = RADIO_TECHNOLOGY_UNKNOWN;
    info.actName = emptyName;
    RilBridge::Get().ReportModem(requestInfo, HRIL_ERR_SUCCESS, &info, sizeof(info));
}

void GetBasebandVersion(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::modem::IRadioModem> modem;
    if (!RilBridge::Get().RequireModem(requestInfo, &modem)) {
        return;
    }
    modem->getBasebandVersion(RilBridge::Get().Track(requestInfo));
}

void ShutDown(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::modem::IRadioModem> modem;
    if (!RilBridge::Get().RequireModem(requestInfo, &modem)) {
        return;
    }
    modem->requestShutdown(RilBridge::Get().Track(requestInfo));
}

const HRilModemReq g_modemOps = {
    .SetRadioState = SetRadioState,
    .GetRadioState = GetRadioState,
    .GetImei = GetImei,
    .GetImeiSv = GetImeiSv,
    .GetMeid = GetMeid,
    .GetVoiceRadioTechnology = GetVoiceRadioTechnology,
    .GetBasebandVersion = GetBasebandVersion,
    .ShutDown = ShutDown,
};

} // namespace

const HRilModemReq *ModemOps()
{
    return &g_modemOps;
}

void AttachModemCallbacks(int32_t slotId,
                          const std::shared_ptr<radio::modem::IRadioModem> &modem)
{
    /* Kept alive for the process lifetime: rild holds only a weak interest in
     * them via setResponseFunctions and we never swap them out. */
    static std::shared_ptr<radio::modem::IRadioModemResponseDelegator> resp[MAX_SLOTS];
    static std::shared_ptr<radio::modem::IRadioModemIndicationDelegator> ind[MAX_SLOTS];

    resp[slotId] = ::ndk::SharedRefBase::make<radio::modem::IRadioModemResponseDelegator>(
        ::ndk::SharedRefBase::make<ModemResponse>());
    ind[slotId] = ::ndk::SharedRefBase::make<radio::modem::IRadioModemIndicationDelegator>(
        ::ndk::SharedRefBase::make<ModemIndication>(slotId));

    auto st = modem->setResponseFunctions(resp[slotId], ind[slotId]);
    if (!st.isOk()) {
        HR_LOGE("modem setResponseFunctions: %{public}s", st.getDescription().c_str());
    }
}

} // namespace HybrisRil
} // namespace OHOS
