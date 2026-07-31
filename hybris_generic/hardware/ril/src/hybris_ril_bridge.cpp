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

#include "hybris_ril_bridge.h"

#include <chrono>
#include <string>
#include <thread>

#include <android/binder_manager.h>

#include "hril_notification.h"
#include "hybris_binder_ndk.h"
#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {

RilBridge &RilBridge::Get()
{
    static RilBridge bridge;
    return bridge;
}

void RilBridge::Start(const struct HRilReport *reportOps)
{
    reportOps_ = reportOps;
    std::thread(&RilBridge::ConnectLoop, this).detach();
}

bool RilBridge::Connected() const
{
    std::lock_guard<std::mutex> guard(lock_);
    return connected_;
}

/*
 * AServiceManager_waitForService blocks until the instance appears, which is
 * exactly what we want on a dedicated thread: rild registers its radio
 * services only once the modem has left `exception` and finished booting,
 * several seconds after the container comes up.
 */
bool RilBridge::ConnectSlot(int32_t slotId)
{
    const std::string suffix = "/slot" + std::to_string(slotId + 1);

    std::string modemName = std::string(radio::modem::IRadioModem::descriptor) + suffix;
    ::ndk::SpAIBinder modemBinder(AServiceManager_waitForService(modemName.c_str()));
    auto modem = radio::modem::IRadioModem::fromBinder(modemBinder);
    if (modem == nullptr) {
        HR_LOGE("no %{public}s", modemName.c_str());
        return false;
    }

    std::string simName = std::string(radio::sim::IRadioSim::descriptor) + suffix;
    ::ndk::SpAIBinder simBinder(AServiceManager_waitForService(simName.c_str()));
    auto sim = radio::sim::IRadioSim::fromBinder(simBinder);
    if (sim == nullptr) {
        HR_LOGE("no %{public}s", simName.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(lock_);
        modem_[slotId] = modem;
        sim_[slotId] = sim;
    }

    AttachModemCallbacks(slotId, modem);
    AttachSimCallbacks(slotId, sim);
    HR_LOGI("slot %{public}d bound to IRadioModem/IRadioSim v2", slotId);
    return true;
}

void RilBridge::ConnectLoop()
{
    if (!BinderNdkInit()) {
        HR_LOGE("libbinder_ndk unavailable — no cellular");
        return;
    }
    /* Two binder threads: responses and indications arrive concurrently and
     * the handlers below are short. */
    BinderNdkStartThreadPool(2);

    for (int32_t slot = 0; slot < MAX_SLOTS; slot++) {
        while (!ConnectSlot(slot)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    {
        std::lock_guard<std::mutex> guard(lock_);
        connected_ = true;
    }

    /*
     * core_service asks for radio state and SIM status long before rild is
     * reachable, and those early requests are answered with an error.  Nudge
     * it to re-query now that the answers are real — the same two
     * notifications a modem reset would produce.
     */
    for (int32_t slot = 0; slot < MAX_SLOTS; slot++) {
        int32_t state = HRIL_RADIO_POWER_STATE_OFF;
        NotifyModem(slot, HNOTI_MODEM_RADIO_STATE_UPDATED, &state, sizeof(state));
        NotifySim(slot, HNOTI_SIM_STATUS_CHANGED, nullptr, 0);
    }
}

std::shared_ptr<radio::modem::IRadioModem> RilBridge::Modem(int32_t slotId)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (slotId < 0 || slotId >= MAX_SLOTS) {
        return nullptr;
    }
    return modem_[slotId];
}

std::shared_ptr<radio::sim::IRadioSim> RilBridge::Sim(int32_t slotId)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (slotId < 0 || slotId >= MAX_SLOTS) {
        return nullptr;
    }
    return sim_[slotId];
}

int32_t RilBridge::Track(const ReqDataInfo *request)
{
    std::lock_guard<std::mutex> guard(pendingLock_);
    int32_t serial = nextSerial_++;
    if (nextSerial_ < 0) {
        nextSerial_ = 1;
    }
    pending_[serial] = const_cast<ReqDataInfo *>(request);
    return serial;
}

ReqDataInfo *RilBridge::TakePending(int32_t serial)
{
    std::lock_guard<std::mutex> guard(pendingLock_);
    auto it = pending_.find(serial);
    if (it == pending_.end()) {
        return nullptr;
    }
    ReqDataInfo *req = it->second;
    pending_.erase(it);
    return req;
}

void RilBridge::ReportModem(const ReqDataInfo *request, int32_t err, const void *data, size_t len)
{
    if (reportOps_ == nullptr || request == nullptr) {
        return;
    }
    struct ReportInfo info = { const_cast<ReqDataInfo *>(request), 0, HRIL_RESPONSE,
                               static_cast<HRilErrNumber>(err), { 0, static_cast<ReportErrorType>(0) },
                               HRIL_UNNEED_ACK };
    reportOps_->OnModemReport(request->slotId, info, static_cast<const uint8_t *>(data), len);
}

void RilBridge::ReportSim(const ReqDataInfo *request, int32_t err, const void *data, size_t len)
{
    if (reportOps_ == nullptr || request == nullptr) {
        return;
    }
    struct ReportInfo info = { const_cast<ReqDataInfo *>(request), 0, HRIL_RESPONSE,
                               static_cast<HRilErrNumber>(err), { 0, static_cast<ReportErrorType>(0) },
                               HRIL_UNNEED_ACK };
    reportOps_->OnSimReport(request->slotId, info, static_cast<const uint8_t *>(data), len);
}

void RilBridge::NotifyModem(int32_t slotId, int32_t notifyId, const void *data, size_t len)
{
    if (reportOps_ == nullptr) {
        return;
    }
    struct ReportInfo info = { nullptr, notifyId, HRIL_NOTIFICATION, HRIL_ERR_SUCCESS,
                               { 0, static_cast<ReportErrorType>(0) }, HRIL_UNNEED_ACK };
    reportOps_->OnModemReport(slotId, info, static_cast<const uint8_t *>(data), len);
}

void RilBridge::NotifySim(int32_t slotId, int32_t notifyId, const void *data, size_t len)
{
    if (reportOps_ == nullptr) {
        return;
    }
    struct ReportInfo info = { nullptr, notifyId, HRIL_NOTIFICATION, HRIL_ERR_SUCCESS,
                               { 0, static_cast<ReportErrorType>(0) }, HRIL_UNNEED_ACK };
    reportOps_->OnSimReport(slotId, info, static_cast<const uint8_t *>(data), len);
}

bool RilBridge::RequireModem(const ReqDataInfo *request,
                             std::shared_ptr<radio::modem::IRadioModem> *out)
{
    if (request == nullptr) {
        return false;
    }
    auto modem = Modem(request->slotId);
    if (modem == nullptr) {
        HR_LOGW("request %{public}d before rild is up", request->request);
        ReportModem(request, HRIL_ERR_GENERIC_FAILURE, nullptr, 0);
        return false;
    }
    *out = modem;
    return true;
}

bool RilBridge::RequireSim(const ReqDataInfo *request, std::shared_ptr<radio::sim::IRadioSim> *out)
{
    if (request == nullptr) {
        return false;
    }
    auto sim = Sim(request->slotId);
    if (sim == nullptr) {
        HR_LOGW("request %{public}d before rild is up", request->request);
        ReportSim(request, HRIL_ERR_GENERIC_FAILURE, nullptr, 0);
        return false;
    }
    *out = sim;
    return true;
}

/*
 * Only the codes the framework acts on differently are mapped; everything
 * else lands on GENERIC_FAILURE, which every caller already handles.
 * Values are android.hardware.radio.RadioError.
 */
int32_t ToHrilError(int32_t radioError)
{
    using RadioError = radio::RadioError;
    switch (static_cast<RadioError>(radioError)) {
        case RadioError::NONE:                  return HRIL_ERR_SUCCESS;
        case RadioError::RADIO_NOT_AVAILABLE:   return HRIL_ERR_MODEM_DEVICE_CLOSE;
        case RadioError::PASSWORD_INCORRECT:    return HRIL_ERR_PINPUK_PASSWORD_NOCORRECT;
        case RadioError::SIM_PIN2:              return HRIL_ERR_NEED_PIN_CODE;
        case RadioError::SIM_PUK2:              return HRIL_ERR_NEED_PUK_CODE;
        case RadioError::REQUEST_NOT_SUPPORTED: return HRIL_ERR_VENDOR_NOT_IMPLEMENT;
        case RadioError::SIM_ABSENT:            return HRIL_ERR_NO_SIMCARD_INSERTED;
        case RadioError::INVALID_ARGUMENTS:     return HRIL_ERR_INVALID_PARAMETER;
        default:                                return HRIL_ERR_GENERIC_FAILURE;
    }
}

} // namespace HybrisRil
} // namespace OHOS
