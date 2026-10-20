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

#include "hybris_gnss_bridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <dirent.h>
#include <unistd.h>

#include <android/binder_manager.h>

#include "parameter.h"

#include "hybris_binder_ndk.h"
#include "hybris_gnss_log.h"

namespace OHOS {
namespace HybrisGnss {
namespace {

using ::OHOS::HybrisRil::BinderNdkInit;
using ::OHOS::HybrisRil::BinderNdkStartThreadPool;

/* Set by androidd once the container serves binder — the gate the camera,
 * audio and RIL bridges use too. */
constexpr const char *CONTAINER_READY_PARAM = "android.composer.ready";

const std::string GNSS_INSTANCE = std::string(gnss::IGnss::descriptor) + "/default";

/* See hybris_ril_bridge.cpp: comm is world-readable, so this is the one
 * cheap way to see container processes at an unprivileged uid. */
bool ContainerProcessUp(const char *want)
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

bool WaitForContainerProcess(const char *comm, int32_t timeoutMs)
{
    constexpr int32_t POLL_MS = 500;
    for (int32_t waited = 0; waited < timeoutMs; waited += POLL_MS) {
        if (ContainerProcessUp(comm)) {
            HG_LOGI("container %{public}s up after %{public}d ms", comm, waited);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    return false;
}

void WaitForContainer()
{
    constexpr int32_t POLL_MS = 500;
    constexpr int32_t TIMEOUT_MS = 180000;
    for (int32_t waited = 0; waited < TIMEOUT_MS; waited += POLL_MS) {
        char value[16] = { 0 };
        if (GetParameter(CONTAINER_READY_PARAM, "0", value, sizeof(value)) > 0 &&
            value[0] == '1') {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    HG_LOGW("%{public}s never set — continuing without it", CONTAINER_READY_PARAM);
}

/*
 * First contact with the container's servicemanager, on a thread we can walk
 * away from.  libbinder's defaultServiceManager() bootstraps under
 * std::call_once and never finishes if it lands before servicemanager owns
 * the context manager — a pegged core for the life of the process, with no
 * way to retry.  Same trap, same backstop as hybris_ril_bridge.cpp.
 */
bool ProbeServiceManager(int32_t timeoutMs)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done]() {
        ::ndk::SpAIBinder ignored(AServiceManager_checkService("hybris.gnss.probe"));
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

void OnBinderDied(void *cookie)
{
    (void)cookie;
    GnssBridge::Get().OnHalDied();
}

int32_t ToAidlMode(int32_t ohosMode)
{
    switch (static_cast<GnssWorkingMode>(ohosMode)) {
        case GnssWorkingMode::GNSS_WORKING_MODE_MS_BASED:
            return static_cast<int32_t>(gnss::IGnss::GnssPositionMode::MS_BASED);
        case GnssWorkingMode::GNSS_WORKING_MODE_MS_ASSISTED:
            return static_cast<int32_t>(gnss::IGnss::GnssPositionMode::MS_ASSISTED);
        default:
            return static_cast<int32_t>(gnss::IGnss::GnssPositionMode::STANDALONE);
    }
}

/* Process-lifetime callback objects: reused across reconnects and
 * re-registrations. */
std::shared_ptr<gnss::IGnssCallback> GnssCallbackObject()
{
    static std::shared_ptr<gnss::IGnssCallback> cb = MakeGnssCallback();
    return cb;
}

std::shared_ptr<gnss::IAGnssCallback> AGnssCallbackObject()
{
    static std::shared_ptr<gnss::IAGnssCallback> cb = MakeAGnssCallback();
    return cb;
}

std::shared_ptr<gnss::IAGnssRilCallback> AGnssRilCallbackObject()
{
    static std::shared_ptr<gnss::IAGnssRilCallback> cb = MakeAGnssRilCallback();
    return cb;
}

} // namespace

/*
 * Never destroyed.  location_host exits when the framework unloads its last
 * device, and destructors run at exit would drop our AIBinder references
 * into the container's libbinder while its binder threads are still live.
 */
GnssBridge &GnssBridge::Get()
{
    static GnssBridge *bridge = new GnssBridge();
    return *bridge;
}

void GnssBridge::Start()
{
    std::lock_guard<std::mutex> guard(lock_);
    if (started_) {
        return;
    }
    started_ = true;
    /* Both threads are created here, on an HDI thread, so they are native
     * OHOS threads — nothing of ours is ever spawned from a binder thread. */
    std::thread(&GnssBridge::DispatchLoop, this).detach();
    std::thread(&GnssBridge::ConnectLoop, this).detach();
}

bool GnssBridge::WaitConnected(int32_t timeoutMs)
{
    Start();
    std::unique_lock<std::mutex> guard(lock_);
    connectedCv_.wait_for(guard, std::chrono::milliseconds(timeoutMs),
                          [this]() { return connected_ || unavailable_; });
    return connected_;
}

bool GnssBridge::Unavailable() const
{
    std::lock_guard<std::mutex> guard(lock_);
    return unavailable_;
}

std::shared_ptr<gnss::IGnss> GnssBridge::Gnss()
{
    std::lock_guard<std::mutex> guard(lock_);
    return connected_ ? gnss_ : nullptr;
}

std::shared_ptr<gnss::IAGnss> GnssBridge::AGnss()
{
    std::lock_guard<std::mutex> guard(lock_);
    return connected_ ? agnss_ : nullptr;
}

std::shared_ptr<gnss::IAGnssRil> GnssBridge::AGnssRil()
{
    std::lock_guard<std::mutex> guard(lock_);
    return connected_ ? agnssRil_ : nullptr;
}

/*
 * The connect thread lives for the whole process: it connects, then sleeps
 * until the HAL dies and connects again.  A thread of our own (musl), never
 * one spawned from the death notification's bionic binder thread.
 */
void GnssBridge::ConnectLoop()
{
    WaitForContainer();

    if (!BinderNdkInit()) {
        HG_LOGE("libbinder_ndk unavailable — no GNSS");
        std::lock_guard<std::mutex> guard(lock_);
        unavailable_ = true;
        connectedCv_.notify_all();
        return;
    }

    constexpr int32_t SM_TIMEOUT_MS = 180000;
    if (!WaitForContainerProcess("servicemanager", SM_TIMEOUT_MS)) {
        HG_LOGW("no container servicemanager after %{public}d ms — trying anyway", SM_TIMEOUT_MS);
    }
    /* servicemanager existing is not enough (it claims the context manager a
     * moment after exec).  mnld is MTK's GNSS daemon, started by the same
     * vendor init stage as the HAL, so by then servicemanager has been
     * answering for a while.  Best-effort; the probe is the backstop. */
    constexpr int32_t MNLD_TIMEOUT_MS = 30000;
    if (!WaitForContainerProcess("mnld", MNLD_TIMEOUT_MS)) {
        HG_LOGW("no mnld after %{public}d ms — probing anyway", MNLD_TIMEOUT_MS);
    }

    constexpr int32_t PROBE_TIMEOUT_MS = 15000;
    if (!ProbeServiceManager(PROBE_TIMEOUT_MS)) {
        HG_LOGE("servicemanager bootstrap wedged — restarting the host");
        _exit(1);
    }

    /* The natural per-device gate: a vendor without the AIDL HAL never
     * declares it, and then there is nothing to wait for. */
    if (!AServiceManager_isDeclared(GNSS_INSTANCE.c_str())) {
        HG_LOGW("%{public}s is not declared by the container — GNSS unavailable",
                GNSS_INSTANCE.c_str());
        std::lock_guard<std::mutex> guard(lock_);
        unavailable_ = true;
        connectedCv_.notify_all();
        return;
    }

    death_ = ::ndk::ScopedAIBinder_DeathRecipient(AIBinder_DeathRecipient_new(OnBinderDied));

    for (;;) {
        if (!Connect()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        ApplySession();

        std::unique_lock<std::mutex> guard(lock_);
        connectedCv_.wait(guard, [this]() { return !connected_; });
        HG_LOGW("IGnss died — reconnecting");
    }
}

bool GnssBridge::Connect()
{
    /* checkService, not waitForService: the latter registers a notification
     * callback and so needs this process serving binder before it holds
     * anything worth serving (hybris_ril_bridge.cpp, AwaitService). */
    static int32_t attempts = 0;
    ::ndk::SpAIBinder binder(AServiceManager_checkService(GNSS_INSTANCE.c_str()));
    if (binder.get() == nullptr) {
        if (attempts++ % 30 == 0) {
            HG_LOGI("waiting for %{public}s", GNSS_INSTANCE.c_str());
        }
        return false;
    }
    auto hal = gnss::IGnss::fromBinder(binder);
    if (hal == nullptr) {
        HG_LOGE("%{public}s is not an IGnss", GNSS_INSTANCE.c_str());
        return false;
    }

    /* Serve from here on: setCallback hands the HAL objects it calls into. */
    static std::once_flag poolOnce;
    std::call_once(poolOnce, []() { BinderNdkStartThreadPool(2); });

    binder_status_t linked = AIBinder_linkToDeath(binder.get(), death_.get(), nullptr);
    if (linked != STATUS_OK) {
        HG_LOGW("linkToDeath failed: %{public}d", linked);
    }

    int32_t version = 0;
    (void)hal->getInterfaceVersion(&version);

    auto gnssCb = GnssCallbackObject();
    auto agnssCb = AGnssCallbackObject();
    auto rilCb = AGnssRilCallbackObject();

    auto st = hal->setCallback(gnssCb);
    if (!st.isOk()) {
        HG_LOGE("IGnss.setCallback: %{public}s", st.getDescription().c_str());
        return false;
    }

    std::shared_ptr<gnss::IAGnss> agnss;
    st = hal->getExtensionAGnss(&agnss);
    if (st.isOk() && agnss != nullptr) {
        st = agnss->setCallback(agnssCb);
        if (!st.isOk()) {
            HG_LOGW("IAGnss.setCallback: %{public}s", st.getDescription().c_str());
        }
    } else {
        HG_LOGW("no IAGnss extension");
        agnss = nullptr;
    }

    std::shared_ptr<gnss::IAGnssRil> agnssRil;
    st = hal->getExtensionAGnssRil(&agnssRil);
    if (st.isOk() && agnssRil != nullptr) {
        st = agnssRil->setCallback(rilCb);
        if (!st.isOk()) {
            HG_LOGW("IAGnssRil.setCallback: %{public}s", st.getDescription().c_str());
        }
    } else {
        HG_LOGW("no IAGnssRil extension");
        agnssRil = nullptr;
    }

    {
        std::lock_guard<std::mutex> guard(lock_);
        gnss_ = hal;
        agnss_ = agnss;
        agnssRil_ = agnssRil;
        connected_ = true;
    }
    connectedCv_.notify_all();
    HG_LOGI("bound to %{public}s v%{public}d (agnss %{public}s, agnss-ril %{public}s)",
            GNSS_INSTANCE.c_str(), version, agnss ? "yes" : "no", agnssRil ? "yes" : "no");
    return true;
}

void GnssBridge::OnHalDied()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        connected_ = false;
        gnss_ = nullptr;
        agnss_ = nullptr;
        agnssRil_ = nullptr;
    }
    connectedCv_.notify_all();
}

void GnssBridge::SetCallbacks(const GnssCallbackStruct &callbacks)
{
    std::lock_guard<std::mutex> guard(lock_);
    callbacks_ = callbacks;
}

void GnssBridge::SetAgnssCallbacks(const AgnssCallbackIfaces &callbacks)
{
    std::lock_guard<std::mutex> guard(lock_);
    agnssCallbacks_ = callbacks;
}

GnssCallbackStruct GnssBridge::Callbacks()
{
    std::lock_guard<std::mutex> guard(lock_);
    return callbacks_;
}

AgnssCallbackIfaces GnssBridge::AgnssCallbacks()
{
    std::lock_guard<std::mutex> guard(lock_);
    return agnssCallbacks_;
}

void GnssBridge::SetPositionConfig(int32_t ohosMode, uint32_t minIntervalMs)
{
    bool running;
    {
        std::lock_guard<std::mutex> guard(lock_);
        positionMode_ = ToAidlMode(ohosMode);
        /* The stock HDI never copies minInterval into the parameter it hands
         * us (gnss_interface_impl.cpp, SetGnssConfigPara), so 0 is the usual
         * value: take it as "the default 1 Hz". */
        if (minIntervalMs != 0) {
            minIntervalMs_ = minIntervalMs;
        }
        running = sessionWanted_;
    }
    /* Android applies a mode change by re-issuing setPositionMode on a live
     * session, without a stop/start; do the same. */
    auto hal = Gnss();
    if (running && hal != nullptr) {
        std::lock_guard<std::mutex> session(sessionLock_);
        gnss::IGnss::PositionModeOptions options;
        {
            std::lock_guard<std::mutex> guard(lock_);
            options.mode = static_cast<gnss::IGnss::GnssPositionMode>(positionMode_);
            options.minIntervalMs = static_cast<int32_t>(minIntervalMs_);
        }
        options.recurrence = gnss::IGnss::GnssPositionRecurrence::RECURRENCE_PERIODIC;
        auto st = hal->setPositionMode(options);
        if (!st.isOk()) {
            HG_LOGW("IGnss.setPositionMode: %{public}s", st.getDescription().c_str());
        }
    }
}

/*
 * Bring the HAL in line with the wanted session.  setPositionMode always
 * precedes start(), as in Android's GnssLocationProvider.  startSvStatus /
 * startNmea are the v2+ opt-ins for those reports; a HAL that predates them
 * just answers UNKNOWN_TRANSACTION, which is harmless.
 */
void GnssBridge::ApplySession()
{
    std::lock_guard<std::mutex> session(sessionLock_);
    bool wanted;
    gnss::IGnss::PositionModeOptions options;
    {
        std::lock_guard<std::mutex> guard(lock_);
        wanted = sessionWanted_;
        options.mode = static_cast<gnss::IGnss::GnssPositionMode>(positionMode_);
        options.minIntervalMs = static_cast<int32_t>(minIntervalMs_);
    }
    auto hal = Gnss();
    if (hal == nullptr) {
        return;
    }
    if (!wanted) {
        (void)hal->stopNmea();
        (void)hal->stopSvStatus();
        auto st = hal->stop();
        if (!st.isOk()) {
            HG_LOGW("IGnss.stop: %{public}s", st.getDescription().c_str());
        }
        HG_LOGI("session stopped");
        return;
    }

    /* The HAL keeps exactly one IGnssCallback, and the last setCallback wins.
     * Any other client (hybris_gnss_test, say) takes the slot over, and when
     * it exits the HAL is left reporting into a dead binder, with nothing for
     * us to notice since the HAL itself never died. Re-claim it at each start
     * so such a takeover lasts at most until the next session. */
    auto st = hal->setCallback(GnssCallbackObject());
    if (!st.isOk()) {
        HG_LOGW("IGnss.setCallback: %{public}s", st.getDescription().c_str());
    }

    options.recurrence = gnss::IGnss::GnssPositionRecurrence::RECURRENCE_PERIODIC;
    options.preferredAccuracyMeters = 0;
    options.preferredTimeMs = 0;
    options.lowPowerMode = false;
    st = hal->setPositionMode(options);
    if (!st.isOk()) {
        HG_LOGW("IGnss.setPositionMode: %{public}s", st.getDescription().c_str());
    }
    st = hal->start();
    if (!st.isOk()) {
        HG_LOGE("IGnss.start: %{public}s", st.getDescription().c_str());
        return;
    }
    (void)hal->startSvStatus();
    (void)hal->startNmea();
    HG_LOGI("session started (mode %{public}d, interval %{public}d ms)",
            static_cast<int32_t>(options.mode), options.minIntervalMs);
}

int32_t GnssBridge::StartSession()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (unavailable_) {
            return -1;
        }
        sessionWanted_ = true;
    }
    ApplySession();
    return 0;
}

int32_t GnssBridge::StopSession()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        sessionWanted_ = false;
    }
    ApplySession();
    return 0;
}

void GnssBridge::Disable()
{
    (void)StopSession();
}

void GnssBridge::Post(std::function<void()> fn)
{
    std::lock_guard<std::mutex> guard(queueLock_);
    /* Bounded: if the OHOS side stops draining (a wedged locationhub), drop
     * the oldest reports rather than grow without limit at NMEA rates. */
    constexpr size_t MAX_QUEUED = 512;
    if (queue_.size() >= MAX_QUEUED) {
        queue_.pop_front();
    }
    queue_.push_back(std::move(fn));
    queueCv_.notify_one();
}

void GnssBridge::DispatchLoop()
{
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> guard(queueLock_);
            queueCv_.wait(guard, [this]() { return !queue_.empty(); });
            fn = std::move(queue_.front());
            queue_.pop_front();
        }
        fn();
    }
}

} // namespace HybrisGnss
} // namespace OHOS
