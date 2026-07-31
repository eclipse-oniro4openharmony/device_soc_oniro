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

/* simOps — the SIM floor of HRilSimReq mapped onto
 * android.hardware.radio.sim v2.  Everything not listed stays NULL, which
 * hril answers with RIL_ERR_VENDOR_NOT_IMPLEMENT. */

#include <memory>
#include <string>
#include <vector>

#include <aidl/android/hardware/radio/sim/AppStatus.h>
#include <aidl/android/hardware/radio/sim/BnRadioSimIndication.h>
#include <aidl/android/hardware/radio/sim/BnRadioSimResponse.h>
#include <aidl/android/hardware/radio/sim/CardStatus.h>
#include <aidl/android/hardware/radio/sim/SimRefreshResult.h>

#include "hril_notification.h"
#include "hybris_ril_bridge.h"
#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {
namespace {

namespace rsim = aidl::android::hardware::radio::sim;

/* The AIDL CardStatus is per-card with a list of applications; HRilCardState
 * is a single flattened record.  Fold the 3GPP application (the one
 * gsmUmtsSubscriptionAppIndex points at) into it, which is what the OHOS
 * framework consumes. */
void ToHrilCardState(const rsim::CardStatus &card, HRilCardState *out, std::string *iccidStore)
{
    out->index = 0;
    out->simType = HRIL_SIM_TYPE_UNKNOWN;
    out->simState = HRIL_SIM_NOT_INSERTED;
    *iccidStore = card.iccid;
    out->iccid = const_cast<char *>(iccidStore->c_str());

    /* CardStatus.cardState: 0 ABSENT, 1 PRESENT, 2 ERROR, 3 RESTRICTED */
    if (card.cardState != 1) {
        out->simState = (card.cardState == 0) ? HRIL_SIM_NOT_INSERTED : HRIL_SIM_NOT_READY;
        return;
    }

    int32_t appIndex = card.gsmUmtsSubscriptionAppIndex;
    if (appIndex < 0 || appIndex >= static_cast<int32_t>(card.applications.size())) {
        /* Card present but no 3GPP application selected yet. */
        out->simState = HRIL_SIM_NOT_READY;
        return;
    }
    const auto &app = card.applications[appIndex];

    switch (app.appType) {
        case rsim::AppStatus::APP_TYPE_SIM:  out->simType = HRIL_SIM_TYPE_SIM; break;
        case rsim::AppStatus::APP_TYPE_USIM: out->simType = HRIL_SIM_TYPE_USIM; break;
        default:                             out->simType = HRIL_SIM_TYPE_UNKNOWN; break;
    }

    switch (app.appState) {
        case rsim::AppStatus::APP_STATE_READY:    out->simState = HRIL_SIM_READY; break;
        case rsim::AppStatus::APP_STATE_PIN:      out->simState = HRIL_SIM_PIN; break;
        case rsim::AppStatus::APP_STATE_PUK:      out->simState = HRIL_SIM_PUK; break;
        case rsim::AppStatus::APP_STATE_DETECTED: out->simState = HRIL_SIM_NOT_READY; break;
        case rsim::AppStatus::APP_STATE_SUBSCRIPTION_PERSO:
            /* Network/SP lock — the sub-state says which; PH_NET_PIN is the
             * one users actually meet on a locked handset. */
            out->simState = HRIL_PH_NET_PIN;
            break;
        default: out->simState = HRIL_SIM_NOT_READY; break;
    }
}

/* ---- responses ------------------------------------------------------- */

class SimResponse : public rsim::IRadioSimResponseDefault {
public:
    ::ndk::ScopedAStatus getIccCardStatusResponse(const radio::RadioResponseInfo &info,
                                                  const rsim::CardStatus &card) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HRilCardState state = {};
        std::string iccid;
        ToHrilCardState(card, &state, &iccid);
        HR_LOGI("card: state=%{public}d type=%{public}d", state.simState, state.simType);
        RilBridge::Get().ReportSim(req, ToHrilError(static_cast<int32_t>(info.error)), &state,
                                   sizeof(state));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getImsiForAppResponse(const radio::RadioResponseInfo &info,
                                               const std::string &imsi) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        RilBridge::Get().ReportSim(req, ToHrilError(static_cast<int32_t>(info.error)),
                                   imsi.c_str(), imsi.size());
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus supplyIccPinForAppResponse(const radio::RadioResponseInfo &info,
                                                    int32_t remainingRetries) override
    {
        return ReportLockStatus(info, remainingRetries);
    }

    ::ndk::ScopedAStatus supplyIccPukForAppResponse(const radio::RadioResponseInfo &info,
                                                    int32_t remainingRetries) override
    {
        return ReportLockStatus(info, remainingRetries);
    }

    ::ndk::ScopedAStatus supplyIccPin2ForAppResponse(const radio::RadioResponseInfo &info,
                                                     int32_t remainingRetries) override
    {
        return ReportLockStatus(info, remainingRetries);
    }

    ::ndk::ScopedAStatus supplyIccPuk2ForAppResponse(const radio::RadioResponseInfo &info,
                                                     int32_t remainingRetries) override
    {
        return ReportLockStatus(info, remainingRetries);
    }

private:
    ::ndk::ScopedAStatus ReportLockStatus(const radio::RadioResponseInfo &info, int32_t remaining)
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HRilLockStatus status = {};
        status.result = (info.error == radio::RadioError::NONE) ? HRIL_UNLOCK_SUCCESS
                                                                : HRIL_UNLOCK_PASSWORD_ERR;
        status.remain = remaining;
        RilBridge::Get().ReportSim(req, ToHrilError(static_cast<int32_t>(info.error)), &status,
                                   sizeof(status));
        return ::ndk::ScopedAStatus::ok();
    }
};

/* ---- indications ----------------------------------------------------- */

class SimIndication : public rsim::IRadioSimIndicationDefault {
public:
    explicit SimIndication(int32_t slotId) : slotId_(slotId) {}

    ::ndk::ScopedAStatus simStatusChanged(radio::RadioIndicationType) override
    {
        HR_LOGI("SIM status changed (slot %{public}d)", slotId_);
        RilBridge::Get().NotifySim(slotId_, HNOTI_SIM_STATUS_CHANGED, nullptr, 0);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus simRefresh(radio::RadioIndicationType,
                                    const rsim::SimRefreshResult &) override
    {
        RilBridge::Get().NotifySim(slotId_, HNOTI_SIM_STATUS_CHANGED, nullptr, 0);
        return ::ndk::ScopedAStatus::ok();
    }

private:
    int32_t slotId_;
};

/* ---- HRilSimReq ------------------------------------------------------ */

void GetSimStatus(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::sim::IRadioSim> sim;
    if (!RilBridge::Get().RequireSim(requestInfo, &sim)) {
        return;
    }
    sim->getIccCardStatus(RilBridge::Get().Track(requestInfo));
}

void GetSimImsi(const ReqDataInfo *requestInfo)
{
    std::shared_ptr<radio::sim::IRadioSim> sim;
    if (!RilBridge::Get().RequireSim(requestInfo, &sim)) {
        return;
    }
    /* Empty aid = the currently selected 3GPP application, which is what the
     * OHOS request means. */
    sim->getImsiForApp(RilBridge::Get().Track(requestInfo), "");
}

void UnlockPin(const ReqDataInfo *requestInfo, const char *pin)
{
    std::shared_ptr<radio::sim::IRadioSim> sim;
    if (!RilBridge::Get().RequireSim(requestInfo, &sim)) {
        return;
    }
    sim->supplyIccPinForApp(RilBridge::Get().Track(requestInfo), pin != nullptr ? pin : "", "");
}

void UnlockPuk(const ReqDataInfo *requestInfo, const char *puk, const char *pin)
{
    std::shared_ptr<radio::sim::IRadioSim> sim;
    if (!RilBridge::Get().RequireSim(requestInfo, &sim)) {
        return;
    }
    sim->supplyIccPukForApp(RilBridge::Get().Track(requestInfo), puk != nullptr ? puk : "",
                            pin != nullptr ? pin : "", "");
}

void UnlockPin2(const ReqDataInfo *requestInfo, const char *pin2)
{
    std::shared_ptr<radio::sim::IRadioSim> sim;
    if (!RilBridge::Get().RequireSim(requestInfo, &sim)) {
        return;
    }
    sim->supplyIccPin2ForApp(RilBridge::Get().Track(requestInfo), pin2 != nullptr ? pin2 : "", "");
}

void UnlockPuk2(const ReqDataInfo *requestInfo, const char *puk2, const char *pin2)
{
    std::shared_ptr<radio::sim::IRadioSim> sim;
    if (!RilBridge::Get().RequireSim(requestInfo, &sim)) {
        return;
    }
    sim->supplyIccPuk2ForApp(RilBridge::Get().Track(requestInfo), puk2 != nullptr ? puk2 : "",
                             pin2 != nullptr ? pin2 : "", "");
}

const HRilSimReq g_simOps = {
    .GetSimStatus = GetSimStatus,
    .GetSimIO = nullptr,
    .GetSimImsi = GetSimImsi,
    .GetSimLockStatus = nullptr,
    .SetSimLock = nullptr,
    .ChangeSimPassword = nullptr,
    .UnlockPin = UnlockPin,
    .UnlockPuk = UnlockPuk,
    .GetSimPinInputTimes = nullptr,
    .UnlockPin2 = UnlockPin2,
    .UnlockPuk2 = UnlockPuk2,
    .GetSimPin2InputTimes = nullptr,
};

} // namespace

const HRilSimReq *SimOps()
{
    return &g_simOps;
}

void AttachSimCallbacks(int32_t slotId, const std::shared_ptr<radio::sim::IRadioSim> &sim)
{
    static std::shared_ptr<rsim::IRadioSimResponseDelegator> resp[MAX_SLOTS];
    static std::shared_ptr<rsim::IRadioSimIndicationDelegator> ind[MAX_SLOTS];

    resp[slotId] = ::ndk::SharedRefBase::make<rsim::IRadioSimResponseDelegator>(
        ::ndk::SharedRefBase::make<SimResponse>());
    ind[slotId] = ::ndk::SharedRefBase::make<rsim::IRadioSimIndicationDelegator>(
        ::ndk::SharedRefBase::make<SimIndication>(slotId));

    auto st = sim->setResponseFunctions(resp[slotId], ind[slotId]);
    if (!st.isOk()) {
        HR_LOGE("sim setResponseFunctions: %{public}s", st.getDescription().c_str());
    }
}

} // namespace HybrisRil
} // namespace OHOS
