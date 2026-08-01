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

#ifndef HYBRIS_RIL_BRIDGE_H
#define HYBRIS_RIL_BRIDGE_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <aidl/android/hardware/radio/AccessNetwork.h>
#include <aidl/android/hardware/radio/data/IRadioData.h>
#include <aidl/android/hardware/radio/messaging/IRadioMessaging.h>
#include <aidl/android/hardware/radio/modem/IRadioModem.h>
#include <aidl/android/hardware/radio/network/IRadioNetwork.h>
#include <aidl/android/hardware/radio/sim/IRadioSim.h>
#include <aidl/android/hardware/radio/voice/IRadioVoice.h>

#include "hril.h"

namespace OHOS {
namespace HybrisRil {

namespace radio = aidl::android::hardware::radio;

/* OHOS runs this product as a single-card build (const.telephony.slotCount=1),
 * so only slot 0 is wired for now; the Android instance names are 1-based
 * ("slot1").  Dual SIM is plan phase R7. */
constexpr int32_t MAX_SLOTS = 1;

/* Set to "1" by androidd once the Halium composer has registered — our
 * proxy for "the container is serving binder".  See WaitForContainer(). */
constexpr const char *CONTAINER_READY_PARAM = "android.composer.ready";

/*
 * The bridge between hril's vendor ABI and the container's AIDL IRadio HAL.
 *
 * hril hands us a request as a `ReqDataInfo *` that we must report against
 * exactly once, then it frees it.  IRadio is the same shape — a client-chosen
 * `serial` echoed back in the response callback — so the whole correlation is
 * one map from our serial to hril's request.
 *
 * Threading: hril dispatches requests serialised under a process-global mutex
 * on binder threads, so vendor ops must never block.  Every op therefore just
 * fires the AIDL call and returns; the answer is reported later from a bionic
 * binder thread inside the response callback.
 */
class RilBridge {
public:
    static RilBridge &Get();

    /* Called from RilInitOps.  Records hril's callbacks and starts the
     * connect thread; never blocks. */
    void Start(const struct HRilReport *reportOps);

    bool Connected() const;

    /* Blocking one-shot lookup, for the domains that hold a service the
     * bridge itself does not keep (hybris_ril_presence.cpp). */
    ::ndk::SpAIBinder AwaitService(const std::string &name);

    std::shared_ptr<radio::modem::IRadioModem> Modem(int32_t slotId);
    std::shared_ptr<radio::sim::IRadioSim> Sim(int32_t slotId);
    std::shared_ptr<radio::network::IRadioNetwork> Network(int32_t slotId);
    std::shared_ptr<radio::messaging::IRadioMessaging> Messaging(int32_t slotId);
    std::shared_ptr<radio::data::IRadioData> Data(int32_t slotId);
    std::shared_ptr<radio::voice::IRadioVoice> Voice(int32_t slotId);

    /* Register an in-flight request and get the serial to pass to IRadio.
     * The ReqDataInfo is owned by hril and handed back by TakePending(). */
    int32_t Track(const ReqDataInfo *request);
    ReqDataInfo *TakePending(int32_t serial);

    /* A serial for a request we make on our own behalf, with no hril request
     * behind it.  The response handler finds nothing pending and drops it. */
    int32_t NextSerial();

    /* Some IRadio indications only say "something changed" where HRil wants
     * the new value (registration is the case that matters).  Issue the
     * matching getter with a serial from TrackNotify(), and the response
     * handler turns the answer into notification `notifyId` instead of a
     * solicited reply. */
    int32_t TrackNotify(int32_t slotId, int32_t notifyId);
    bool TakeNotify(int32_t serial, int32_t *slotId, int32_t *notifyId);

    /* Solicited responses.  `err` is an HRilErrNumber; data/len are the
     * domain payload hril expects (NULL/0 when the request has none). */
    void ReportModem(const ReqDataInfo *request, int32_t err, const void *data, size_t len);
    void ReportSim(const ReqDataInfo *request, int32_t err, const void *data, size_t len);
    void ReportNetwork(const ReqDataInfo *request, int32_t err, const void *data, size_t len);
    void ReportSms(const ReqDataInfo *request, int32_t err, const void *data, size_t len);
    void ReportData(const ReqDataInfo *request, int32_t err, const void *data, size_t len);
    void ReportCall(const ReqDataInfo *request, int32_t err, const void *data, size_t len);

    /* Unsolicited notifications (requestInfo == NULL + notifyId). */
    void NotifyModem(int32_t slotId, int32_t notifyId, const void *data, size_t len);
    void NotifySim(int32_t slotId, int32_t notifyId, const void *data, size_t len);
    void NotifyNetwork(int32_t slotId, int32_t notifyId, const void *data, size_t len);
    void NotifySms(int32_t slotId, int32_t notifyId, const void *data, size_t len);
    void NotifyData(int32_t slotId, int32_t notifyId, const void *data, size_t len);
    void NotifyCall(int32_t slotId, int32_t notifyId, const void *data, size_t len);

    /* Fail an op that arrived before the AIDL side was up.  Reports
     * HRIL_ERR_GENERIC_FAILURE so the framework retries rather than wedging. */
    bool RequireModem(const ReqDataInfo *request, std::shared_ptr<radio::modem::IRadioModem> *out);
    bool RequireSim(const ReqDataInfo *request, std::shared_ptr<radio::sim::IRadioSim> *out);
    bool RequireNetwork(const ReqDataInfo *request,
                        std::shared_ptr<radio::network::IRadioNetwork> *out);
    bool RequireSms(const ReqDataInfo *request,
                    std::shared_ptr<radio::messaging::IRadioMessaging> *out);
    bool RequireData(const ReqDataInfo *request, std::shared_ptr<radio::data::IRadioData> *out);
    bool RequireVoice(const ReqDataInfo *request, std::shared_ptr<radio::voice::IRadioVoice> *out);

private:
    RilBridge() = default;
    void WaitForContainer();
    void WaitForServiceManager();
    void ConnectLoop();
    bool ConnectSlot(int32_t slotId);

    const struct HRilReport *reportOps_ = nullptr;

    mutable std::mutex lock_;
    std::shared_ptr<radio::modem::IRadioModem> modem_[MAX_SLOTS];
    std::shared_ptr<radio::sim::IRadioSim> sim_[MAX_SLOTS];
    std::shared_ptr<radio::network::IRadioNetwork> network_[MAX_SLOTS];
    std::shared_ptr<radio::messaging::IRadioMessaging> messaging_[MAX_SLOTS];
    std::shared_ptr<radio::data::IRadioData> data_[MAX_SLOTS];
    std::shared_ptr<radio::voice::IRadioVoice> voice_[MAX_SLOTS];
    bool connected_ = false;

    struct NotifyTarget {
        int32_t slotId;
        int32_t notifyId;
    };

    std::mutex pendingLock_;
    std::map<int32_t, ReqDataInfo *> pending_;
    std::map<int32_t, NotifyTarget> notifying_;
    int32_t nextSerial_ = 1;
};

/* AIDL RadioError -> HRilErrNumber, with a conservative default. */
int32_t ToHrilError(int32_t radioError);

/* The op tables, defined per domain. */
const HRilModemReq *ModemOps();
const HRilSimReq *SimOps();
const HRilNetworkReq *NetworkOps();
const HRilSmsReq *SmsOps();
const HRilDataReq *DataOps();
const HRilCallReq *CallOps();

/* Response/indication objects, created once per slot by the bridge. */
void AttachModemCallbacks(int32_t slotId, const std::shared_ptr<radio::modem::IRadioModem> &modem);
void AttachSimCallbacks(int32_t slotId, const std::shared_ptr<radio::sim::IRadioSim> &sim);
void AttachNetworkCallbacks(int32_t slotId,
                            const std::shared_ptr<radio::network::IRadioNetwork> &network);
void AttachMessagingCallbacks(int32_t slotId,
                              const std::shared_ptr<radio::messaging::IRadioMessaging> &messaging);
void AttachDataCallbacks(int32_t slotId, const std::shared_ptr<radio::data::IRadioData> &data);
void AttachVoiceCallbacks(int32_t slotId, const std::shared_ptr<radio::voice::IRadioVoice> &voice);

/* The access network the *data* registration last reported, kept by the
 * network domain.  The framework's own radio-technology field is unreliable
 * on this port; see AccessNetworkFor() in hybris_ril_data.cpp. */
radio::AccessNetwork LastDataAccessNetwork(int32_t slotId);

/* Registers the interfaces we do not implement yet (ims).  Not optional: MTK
 * gates MT SMS on all seven being registered — see hybris_ril_presence.cpp. */
bool AttachPresenceCallbacks(int32_t slotId);

} // namespace HybrisRil
} // namespace OHOS

#endif // HYBRIS_RIL_BRIDGE_H
