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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <dirent.h>
#include <unistd.h>

#include <android/binder_manager.h>

#include "parameter.h"

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
 * AServiceManager_checkService in a poll loop rather than waitForService.
 *
 * waitForService is the obvious call — rild only registers its radio
 * services once the modem has left `exception`, several seconds after the
 * container comes up — but it registers a service-notification callback with
 * servicemanager, which requires this process to be able to *serve* incoming
 * binder transactions before it has any reason to.  checkService is a plain
 * one-shot lookup that returns null when the service is absent, needs
 * nothing served back, and costs one binder round trip a second while we
 * wait, so the thread pool can stay unstarted until we hold a handle.
 *
 * This is not what protects us from the pegged-core wedge: that one lives in
 * defaultServiceManager(), which both calls go through — see
 * ProbeServiceManager() below.
 */
::ndk::SpAIBinder RilBridge::AwaitService(const std::string &name)
{
    constexpr int32_t POLL_MS = 1000;
    constexpr int32_t TIMEOUT_MS = 300000;
    for (int32_t waited = 0; waited < TIMEOUT_MS; waited += POLL_MS) {
        ::ndk::SpAIBinder binder(AServiceManager_checkService(name.c_str()));
        if (binder.get() != nullptr) {
            return binder;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    HR_LOGE("%{public}s never appeared", name.c_str());
    return ::ndk::SpAIBinder();
}

bool RilBridge::ConnectSlot(int32_t slotId)
{
    const std::string suffix = "/slot" + std::to_string(slotId + 1);

    std::string modemName = std::string(radio::modem::IRadioModem::descriptor) + suffix;
    auto modem = radio::modem::IRadioModem::fromBinder(AwaitService(modemName));
    if (modem == nullptr) {
        HR_LOGE("no %{public}s", modemName.c_str());
        return false;
    }

    std::string netName = std::string(radio::network::IRadioNetwork::descriptor) + suffix;
    auto network = radio::network::IRadioNetwork::fromBinder(AwaitService(netName));
    if (network == nullptr) {
        HR_LOGE("no %{public}s", netName.c_str());
        return false;
    }

    std::string simName = std::string(radio::sim::IRadioSim::descriptor) + suffix;
    auto sim = radio::sim::IRadioSim::fromBinder(AwaitService(simName));
    if (sim == nullptr) {
        HR_LOGE("no %{public}s", simName.c_str());
        return false;
    }

    std::string msgName = std::string(radio::messaging::IRadioMessaging::descriptor) + suffix;
    auto messaging = radio::messaging::IRadioMessaging::fromBinder(AwaitService(msgName));
    if (messaging == nullptr) {
        HR_LOGE("no %{public}s", msgName.c_str());
        return false;
    }

    std::string dataName = std::string(radio::data::IRadioData::descriptor) + suffix;
    auto data = radio::data::IRadioData::fromBinder(AwaitService(dataName));
    if (data == nullptr) {
        HR_LOGE("no %{public}s", dataName.c_str());
        return false;
    }

    /* Only now start serving: setResponseFunctions below hands rild binder
     * objects it will call back into, so the pool has to exist — but not one
     * moment earlier than that. */
    BinderNdkStartThreadPool(2);

    {
        std::lock_guard<std::mutex> guard(lock_);
        modem_[slotId] = modem;
        sim_[slotId] = sim;
        network_[slotId] = network;
        messaging_[slotId] = messaging;
        data_[slotId] = data;
    }

    AttachModemCallbacks(slotId, modem);
    AttachSimCallbacks(slotId, sim);
    AttachNetworkCallbacks(slotId, network);
    AttachMessagingCallbacks(slotId, messaging);
    AttachDataCallbacks(slotId, data);

    /* The remaining two of the seven interfaces MTK counts before it
     * considers the framework connected.  Registering only what we use
     * leaves inbound SMS undeliverable — see hybris_ril_presence.cpp. */
    AttachPresenceCallbacks(slotId);

    HR_LOGI("slot %{public}d bound to "
            "IRadioModem/IRadioSim/IRadioNetwork/IRadioMessaging/IRadioData v2", slotId);
    return true;
}

/*
 * Is a container process with this name running?
 *
 * The container has its own PID namespace but we are its parent, so its
 * processes are visible here under host PIDs.  comm is world-readable,
 * which the fd list of a root-owned process is not — so this is the only
 * cheap probe available to us at the riladapter_host uid.
 */
static bool ContainerProcessUp(const char *want)
{
    DIR *proc = opendir("/proc");
    if (proc == nullptr) {
        return false;
    }
    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(proc)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        char path[64];
        (void)snprintf(path, sizeof path, "/proc/%s/comm", ent->d_name);
        FILE *f = fopen(path, "re");
        if (f == nullptr) {
            continue;
        }
        char comm[64] = { 0 };
        if (fgets(comm, sizeof comm, f) != nullptr) {
            comm[strcspn(comm, "\n")] = '\0';
            found = (strcmp(comm, want) == 0);
        }
        (void)fclose(f);
    }
    (void)closedir(proc);
    return found;
}

static bool WaitForContainerProcess(const char *comm, int32_t timeoutMs)
{
    constexpr int32_t POLL_MS = 500;
    for (int32_t waited = 0; waited < timeoutMs; waited += POLL_MS) {
        if (ContainerProcessUp(comm)) {
            HR_LOGI("container %{public}s up after %{public}d ms", comm, waited);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    return false;
}

/*
 * Make first contact with the container's servicemanager somewhere we can
 * walk away from.
 *
 * libbinder caches the servicemanager proxy under std::call_once, and the
 * initialiser retries internally until it gets a context object.  Called
 * before servicemanager has claimed the context manager, that retry never
 * completes — and because the once-flag is already taken, no later call can
 * redo it.  The result is a thread spinning at 100% inside
 * android::defaultServiceManager() for the rest of the uptime, with the
 * rest of the process working fine and nothing in our own logs.  Every
 * AServiceManager_* entry point goes through it, so choosing checkService
 * over waitForService does not avoid this.
 *
 * There is no way to cancel a wedged call_once, so the only recovery is a
 * fresh process: probe on a detached thread, and if it has not returned by
 * the deadline, exit and let hdf_devmgr restart us.
 */
static bool ProbeServiceManager(int32_t timeoutMs)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done]() {
        /* Any name will do — this exists to force the one-time bootstrap,
         * and an absent service is the cheapest possible lookup. */
        ::ndk::SpAIBinder ignored(AServiceManager_checkService("hybris.ril.probe"));
        (void)ignored;
        done->store(true);
    }).detach();

    constexpr int32_t POLL_MS = 100;
    for (int32_t waited = 0; waited < timeoutMs; waited += POLL_MS) {
        if (done->load()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    return false;
}

/*
 * Wait for the Halium container to be usable before touching binder.
 *
 * riladapter_host starts with the rest of the HDF hosts, well before
 * androidd has finished bringing up the container, and joining the binder
 * thread pool against a context manager that does not exist yet leaves a
 * bionic binder thread spinning in the driver — one core pegged for the
 * whole uptime, which is how this was found.  androidd flips
 * `android.composer.ready` when the Halium composer registers, which is the
 * same "container is serving binder" signal the camera and audio bridges
 * gate on.
 */
void RilBridge::WaitForContainer()
{
    constexpr int32_t POLL_MS = 500;
    constexpr int32_t TIMEOUT_MS = 180000;
    for (int32_t waited = 0; waited < TIMEOUT_MS; waited += POLL_MS) {
        char value[16] = { 0 };
        if (GetParameter(CONTAINER_READY_PARAM, "0", value, sizeof(value)) > 0 &&
            value[0] == '1') {
            HR_LOGI("container ready after %{public}d ms", waited);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    /* Carry on anyway: a container that never announced itself may still
     * come up, and blocking here forever guarantees no cellular. */
    HR_LOGW("%{public}s never set — continuing without it", CONTAINER_READY_PARAM);
}

/*
 * The gate that actually matters for binder: the AIDL servicemanager.
 *
 * `android.composer.ready` does NOT imply it.  androidd sets that from an
 * `lshal` probe, which queries **hwservicemanager** — the HIDL daemon on
 * /dev/hwbinder.  The AIDL servicemanager is a different process, owning
 * the context manager on /dev/binderfs/android-binder, and the two are not
 * ordered against each other: on a cold boot riladapter_host starts at
 * ~4.9 s and servicemanager at ~6.5 s, so the composer gate can open while
 * nothing is yet answering on the node we actually use.
 */
void RilBridge::WaitForServiceManager()
{
    constexpr int32_t SM_TIMEOUT_MS = 180000;
    if (!WaitForContainerProcess("servicemanager", SM_TIMEOUT_MS)) {
        HR_LOGW("no container servicemanager after %{public}d ms — trying anyway", SM_TIMEOUT_MS);
        return;
    }

    /*
     * servicemanager existing is necessary but not sufficient: it claims the
     * context manager shortly after exec, and a probe that lands in that
     * window still wedges (observed on a cold boot even with a 500 ms
     * settle).  rild is the cheap sufficient signal — it is a binder client
     * of servicemanager, so if it is running, servicemanager has been
     * answering for a while.  Best-effort: if rild never appears we still
     * go on, and ProbeServiceManager() is the backstop either way.
     */
    constexpr int32_t RILD_TIMEOUT_MS = 60000;
    if (!WaitForContainerProcess("mtkfusionrild", RILD_TIMEOUT_MS)) {
        HR_LOGW("no mtkfusionrild after %{public}d ms — probing anyway", RILD_TIMEOUT_MS);
    }
}

void RilBridge::ConnectLoop()
{
    WaitForContainer();

    if (!BinderNdkInit()) {
        HR_LOGE("libbinder_ndk unavailable — no cellular");
        return;
    }

    WaitForServiceManager();

    /* One shot, and it is the whole process's shot — see
     * ProbeServiceManager().  A healthy bootstrap takes milliseconds, and
     * a lookup for an absent service returns just as fast once
     * servicemanager answers, so the only slow case is the wedge: keep the
     * deadline short so a lost race costs one restart, not a burnt core. */
    constexpr int32_t PROBE_TIMEOUT_MS = 15000;
    if (!ProbeServiceManager(PROBE_TIMEOUT_MS)) {
        HR_LOGE("servicemanager bootstrap wedged — restarting the host");
        /* _exit, not exit: the wedged thread is inside the container's
         * libbinder and static destructors would run against it. */
        _exit(1);
    }
    /* The thread pool is started later, once we actually hold a service
     * handle — see ConnectSlot(). */
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
     * it to re-query now that the answers are real.
     *
     * The radio-state notification says OFF, and that is deliberate.
     * NetworkSearchManager caches this value and never polls the vendor for
     * it; left at its initial NOT_AVAILABLE the state machine simply waits
     * for a modem that, as far as it knows, is not there.  Told OFF, it
     * checks airplane mode, decides the radio should be on, and calls
     * SetRadioState(1) — at which point the setRadioPower response reports
     * the real transition to ON (hybris_ril_modem.cpp).  IRadio v2 has no
     * getRadioState, so asserting OFF and letting the framework correct us
     * is the only way to get an accurate answer.
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

std::shared_ptr<radio::network::IRadioNetwork> RilBridge::Network(int32_t slotId)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (slotId < 0 || slotId >= MAX_SLOTS) {
        return nullptr;
    }
    return network_[slotId];
}

std::shared_ptr<radio::messaging::IRadioMessaging> RilBridge::Messaging(int32_t slotId)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (slotId < 0 || slotId >= MAX_SLOTS) {
        return nullptr;
    }
    return messaging_[slotId];
}

std::shared_ptr<radio::data::IRadioData> RilBridge::Data(int32_t slotId)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (slotId < 0 || slotId >= MAX_SLOTS) {
        return nullptr;
    }
    return data_[slotId];
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

int32_t RilBridge::NextSerial()
{
    std::lock_guard<std::mutex> guard(pendingLock_);
    int32_t serial = nextSerial_++;
    if (nextSerial_ < 0) {
        nextSerial_ = 1;
    }
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

int32_t RilBridge::TrackNotify(int32_t slotId, int32_t notifyId)
{
    std::lock_guard<std::mutex> guard(pendingLock_);
    int32_t serial = nextSerial_++;
    if (nextSerial_ < 0) {
        nextSerial_ = 1;
    }
    notifying_[serial] = { slotId, notifyId };
    return serial;
}

bool RilBridge::TakeNotify(int32_t serial, int32_t *slotId, int32_t *notifyId)
{
    std::lock_guard<std::mutex> guard(pendingLock_);
    auto it = notifying_.find(serial);
    if (it == notifying_.end()) {
        return false;
    }
    *slotId = it->second.slotId;
    *notifyId = it->second.notifyId;
    notifying_.erase(it);
    return true;
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

void RilBridge::ReportNetwork(const ReqDataInfo *request, int32_t err, const void *data,
                              size_t len)
{
    if (reportOps_ == nullptr || request == nullptr) {
        return;
    }
    struct ReportInfo info = { const_cast<ReqDataInfo *>(request), 0, HRIL_RESPONSE,
                               static_cast<HRilErrNumber>(err), { 0, static_cast<ReportErrorType>(0) },
                               HRIL_UNNEED_ACK };
    reportOps_->OnNetworkReport(request->slotId, info, static_cast<const uint8_t *>(data), len);
}

void RilBridge::ReportSms(const ReqDataInfo *request, int32_t err, const void *data, size_t len)
{
    if (reportOps_ == nullptr || request == nullptr) {
        return;
    }
    struct ReportInfo info = { const_cast<ReqDataInfo *>(request), 0, HRIL_RESPONSE,
                               static_cast<HRilErrNumber>(err), { 0, static_cast<ReportErrorType>(0) },
                               HRIL_UNNEED_ACK };
    reportOps_->OnSmsReport(request->slotId, info, static_cast<const uint8_t *>(data), len);
}

void RilBridge::ReportData(const ReqDataInfo *request, int32_t err, const void *data, size_t len)
{
    if (reportOps_ == nullptr || request == nullptr) {
        return;
    }
    struct ReportInfo info = { const_cast<ReqDataInfo *>(request), 0, HRIL_RESPONSE,
                               static_cast<HRilErrNumber>(err), { 0, static_cast<ReportErrorType>(0) },
                               HRIL_UNNEED_ACK };
    reportOps_->OnDataReport(request->slotId, info, static_cast<const uint8_t *>(data), len);
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

void RilBridge::NotifyNetwork(int32_t slotId, int32_t notifyId, const void *data, size_t len)
{
    if (reportOps_ == nullptr) {
        return;
    }
    struct ReportInfo info = { nullptr, notifyId, HRIL_NOTIFICATION, HRIL_ERR_SUCCESS,
                               { 0, static_cast<ReportErrorType>(0) }, HRIL_UNNEED_ACK };
    reportOps_->OnNetworkReport(slotId, info, static_cast<const uint8_t *>(data), len);
}

void RilBridge::NotifySms(int32_t slotId, int32_t notifyId, const void *data, size_t len)
{
    if (reportOps_ == nullptr) {
        return;
    }
    struct ReportInfo info = { nullptr, notifyId, HRIL_NOTIFICATION, HRIL_ERR_SUCCESS,
                               { 0, static_cast<ReportErrorType>(0) }, HRIL_UNNEED_ACK };
    reportOps_->OnSmsReport(slotId, info, static_cast<const uint8_t *>(data), len);
}

void RilBridge::NotifyData(int32_t slotId, int32_t notifyId, const void *data, size_t len)
{
    if (reportOps_ == nullptr) {
        return;
    }
    struct ReportInfo info = { nullptr, notifyId, HRIL_NOTIFICATION, HRIL_ERR_SUCCESS,
                               { 0, static_cast<ReportErrorType>(0) }, HRIL_UNNEED_ACK };
    reportOps_->OnDataReport(slotId, info, static_cast<const uint8_t *>(data), len);
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

bool RilBridge::RequireNetwork(const ReqDataInfo *request,
                               std::shared_ptr<radio::network::IRadioNetwork> *out)
{
    if (request == nullptr) {
        return false;
    }
    auto network = Network(request->slotId);
    if (network == nullptr) {
        HR_LOGW("request %{public}d before rild is up", request->request);
        ReportNetwork(request, HRIL_ERR_GENERIC_FAILURE, nullptr, 0);
        return false;
    }
    *out = network;
    return true;
}

bool RilBridge::RequireSms(const ReqDataInfo *request,
                           std::shared_ptr<radio::messaging::IRadioMessaging> *out)
{
    if (request == nullptr) {
        return false;
    }
    auto messaging = Messaging(request->slotId);
    if (messaging == nullptr) {
        HR_LOGW("request %{public}d before rild is up", request->request);
        ReportSms(request, HRIL_ERR_GENERIC_FAILURE, nullptr, 0);
        return false;
    }
    *out = messaging;
    return true;
}

bool RilBridge::RequireData(const ReqDataInfo *request,
                            std::shared_ptr<radio::data::IRadioData> *out)
{
    if (request == nullptr) {
        return false;
    }
    auto data = Data(request->slotId);
    if (data == nullptr) {
        HR_LOGW("request %{public}d before rild is up", request->request);
        ReportData(request, HRIL_ERR_GENERIC_FAILURE, nullptr, 0);
        return false;
    }
    *out = data;
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
