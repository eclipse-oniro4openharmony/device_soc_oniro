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
#include <mutex>
#include <string>
#include <vector>

#include <aidl/android/hardware/radio/sim/AppStatus.h>
#include <aidl/android/hardware/radio/sim/BnRadioSimIndication.h>
#include <aidl/android/hardware/radio/sim/BnRadioSimResponse.h>
#include <aidl/android/hardware/radio/sim/CardStatus.h>
#include <aidl/android/hardware/radio/sim/IccIo.h>
#include <aidl/android/hardware/radio/sim/IccIoResult.h>
#include <aidl/android/hardware/radio/sim/SimRefreshResult.h>

#include "hril_notification.h"
#include "hybris_ril_bridge.h"
#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {
namespace {

namespace rsim = aidl::android::hardware::radio::sim;

/*
 * The AID of the 3GPP application on the card, learned from the card status.
 *
 * OHOS's SimIoRequestInfo has no aid field — its file controllers address
 * everything by path — but IccIo does, and on a USIM the elementary files
 * that matter live under ADF_USIM, which cannot be selected without it.
 * Android's own framework passes the AID from UiccCardApplication for
 * exactly this reason.  Cache it from getIccCardStatusResponse, which the
 * framework issues before it reads any file.
 */
std::mutex g_aidLock;
std::string g_appAid[MAX_SLOTS];

void RememberAppAid(int32_t slotId, const std::string &aid)
{
    if (slotId < 0 || slotId >= MAX_SLOTS) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_aidLock);
    g_appAid[slotId] = aid;
}

std::string AppAid(int32_t slotId)
{
    if (slotId < 0 || slotId >= MAX_SLOTS) {
        return "";
    }
    std::lock_guard<std::mutex> guard(g_aidLock);
    return g_appAid[slotId];
}

/* The AIDL CardStatus is per-card with a list of applications; HRilCardState
 * is a single flattened record.  Fold the 3GPP application (the one
 * gsmUmtsSubscriptionAppIndex points at) into it, which is what the OHOS
 * framework consumes. */
void ToHrilCardState(const rsim::CardStatus &card, HRilCardState *out, std::string *iccidStore,
                     std::string *aidStore)
{
    aidStore->clear();
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
    *aidStore = app.aidPtr;

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
    explicit SimResponse(int32_t slotId) : slotId_(slotId) {}

    ::ndk::ScopedAStatus getIccCardStatusResponse(const radio::RadioResponseInfo &info,
                                                  const rsim::CardStatus &card) override
    {
        HRilCardState state = {};
        std::string iccid;
        std::string aid;
        ToHrilCardState(card, &state, &iccid, &aid);
        /* Unconditionally, before the early return below: the framework polls
         * card status through paths that do not always leave a pending
         * request, and the AID is what every later file read depends on. */
        RememberAppAid(slotId_, aid);

        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HR_LOGI("card: state=%{public}d type=%{public}d aid=%{public}zu chars", state.simState,
                state.simType, aid.size());
        RilBridge::Get().ReportSim(req, ToHrilError(static_cast<int32_t>(info.error)), &state,
                                   sizeof(state));
        return ::ndk::ScopedAStatus::ok();
    }

    /*
     * Every elementary-file read the SIM file manager makes lands here.
     * sw1/sw2 are the card's own status words and are carried through
     * untouched rather than folded into the RIL error: SimStateManager's
     * GetSimIO hands them to its caller, and the eSIM code reads them
     * directly (SW1_VALUE_90).  Only a transport failure belongs in the
     * error field.
     */
    ::ndk::ScopedAStatus iccIoForAppResponse(const radio::RadioResponseInfo &info,
                                             const rsim::IccIoResult &result) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HRilSimIOResponse response = {};
        response.sw1 = result.sw1;
        response.sw2 = result.sw2;
        response.response = const_cast<char *>(result.simResponse.c_str());
        HR_LOGD("sim io: err=%{public}d sw1=%{public}d sw2=%{public}d len=%{public}zu",
                static_cast<int32_t>(info.error), result.sw1, result.sw2,
                result.simResponse.size());
        RilBridge::Get().ReportSim(req, ToHrilError(static_cast<int32_t>(info.error)), &response,
                                   sizeof(response));
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus enableUiccApplicationsResponse(
        const radio::RadioResponseInfo &info) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HR_LOGI("set active sim: err=%{public}d", static_cast<int32_t>(info.error));
        RilBridge::Get().ReportSim(req, ToHrilError(static_cast<int32_t>(info.error)), nullptr, 0);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getImsiForAppResponse(const radio::RadioResponseInfo &info,
                                               const std::string &imsi) override
    {
        ReqDataInfo *req = RilBridge::Get().TakePending(info.serial);
        if (req == nullptr) {
            return ::ndk::ScopedAStatus::ok();
        }
        HR_LOGI("imsi: err=%{public}d %{public}zu digits", static_cast<int32_t>(info.error),
                imsi.size());
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

    int32_t slotId_;
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
    /* Same AID as the file reads.  An empty string is documented to mean
     * "the current application", but MTK's rild answers it with an empty
     * IMSI, and an empty IMSI means SimFile never derives the operator
     * numeric — which is what cellular_data keys its APN lookup on. */
    sim->getImsiForApp(RilBridge::Get().Track(requestInfo), AppAid(requestInfo->slotId));
}

/*
 * SIM file I/O — the request the whole SIM-records chain hangs off.
 *
 * Without it SimFile never finishes ObtainAllFiles, so core_service never
 * raises RADIO_SIM_RECORDS_LOADED, so cellular_data never runs
 * CreateAllApnItemByDatabase and has no APN to connect with.  Nothing in
 * either log says so: the connection state machine simply reports
 * "matchedApns is empty" forever.
 *
 * `dataLen` is sizeof(HRilSimIO); HRilSim::GetSimIO always passes the whole
 * struct.
 */
void GetSimIO(const ReqDataInfo *requestInfo, const HRilSimIO *data, size_t dataLen)
{
    std::shared_ptr<radio::sim::IRadioSim> sim;
    if (!RilBridge::Get().RequireSim(requestInfo, &sim)) {
        return;
    }
    if (data == nullptr || dataLen < sizeof(HRilSimIO)) {
        HR_LOGE("GetSimIO: bad payload");
        RilBridge::Get().ReportSim(requestInfo, HRIL_ERR_INVALID_PARAMETER, nullptr, 0);
        return;
    }

    rsim::IccIo io;
    io.command = data->command;
    io.fileId = data->fileid;
    io.path = (data->pathid != nullptr) ? data->pathid : "";
    io.p1 = data->p1;
    io.p2 = data->p2;
    io.p3 = data->p3;
    io.data = (data->data != nullptr) ? data->data : "";
    io.pin2 = (data->pin2 != nullptr) ? data->pin2 : "";
    io.aid = AppAid(requestInfo->slotId);

    HR_LOGD("sim io: cmd=%{public}d file=%{public}d path=%{public}s p=%{public}d/%{public}d/"
            "%{public}d", io.command, io.fileId, io.path.c_str(), io.p1, io.p2, io.p3);
    sim->iccIoForApp(RilBridge::Get().Track(requestInfo), io);
}

/*
 * "Activate this SIM" — the gate on the whole SIM-account chain.
 *
 * MultiSimController::InitActive calls this before it will publish the SIM
 * account; a failure (including RIL_ERR_VENDOR_NOT_IMPLEMENT, which is what
 * a NULL op returns) makes MultiSimMonitor::InitData fail and retry
 * eleven times, ten seconds apart, and then stop.  RADIO_SIM_ACCOUNT_LOADED
 * never fires, so cellular_data never registers its net supplier and no
 * data connection is ever requested — with a fully working SIM, network
 * registration and APN list.  The only clue is one line from hril:
 * "reqFunSet or reqFuncSet->*fun is null".
 *
 * `index` is the slot, which the AIDL object already implies.
 */
void SetActiveSim(const ReqDataInfo *requestInfo, int32_t index, int32_t enable)
{
    std::shared_ptr<radio::sim::IRadioSim> sim;
    if (!RilBridge::Get().RequireSim(requestInfo, &sim)) {
        return;
    }
    HR_LOGI("set active sim: slot=%{public}d enable=%{public}d", index, enable);
    sim->enableUiccApplications(RilBridge::Get().Track(requestInfo), enable != 0);
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
    .GetSimIO = GetSimIO,
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
    .SetActiveSim = SetActiveSim,
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
        ::ndk::SharedRefBase::make<SimResponse>(slotId));
    ind[slotId] = ::ndk::SharedRefBase::make<rsim::IRadioSimIndicationDelegator>(
        ::ndk::SharedRefBase::make<SimIndication>(slotId));

    auto st = sim->setResponseFunctions(resp[slotId], ind[slotId]);
    if (!st.isOk()) {
        HR_LOGE("sim setResponseFunctions: %{public}s", st.getDescription().c_str());
    }
}

} // namespace HybrisRil
} // namespace OHOS
