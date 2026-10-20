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

#ifndef HYBRIS_GNSS_BRIDGE_H
#define HYBRIS_GNSS_BRIDGE_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <android/binder_auto_utils.h>

#include <aidl/android/hardware/gnss/IAGnss.h>
#include <aidl/android/hardware/gnss/IAGnssRil.h>
#include <aidl/android/hardware/gnss/IGnss.h>

#include "location_vendor_lib.h"

namespace OHOS {
namespace HybrisGnss {

namespace gnss = ::aidl::android::hardware::gnss;

/* The vendor ABI's structs and enums.  Unqualified names in this module are
 * always the OHOS ones; the AIDL ones are always spelled gnss::. */
using namespace ::OHOS::HDI::Location;

/*
 * Connection to the container's android.hardware.gnss.IGnss/default (AIDL
 * v3, android.hardware.gnss-service.mediatek) plus the session state the
 * OHOS side has asked for.
 *
 * The OHOS GNSS HDI is stateful in the order enable → config → start, and
 * it may issue that sequence before the container's HAL is reachable (the
 * location switch persists across reboots).  So every vendor op records
 * the *desired* state here and applies it when it can; a (re)connect replays
 * it, which also covers the HAL restarting under us.
 */
class GnssBridge {
public:
    static GnssBridge &Get();

    /* Start the connect thread if it is not already running.  Never blocks. */
    void Start();

    /* Wait up to timeoutMs for a usable IGnss.  False on timeout, and also —
     * immediately — once the HAL is known not to exist on this device. */
    bool WaitConnected(int32_t timeoutMs);

    /* True once the connect thread has given up for good (no AIDL GNSS HAL
     * declared — e.g. a device whose vendor ships only the HIDL one). */
    bool Unavailable() const;

    std::shared_ptr<gnss::IGnss> Gnss();
    std::shared_ptr<gnss::IAGnss> AGnss();
    std::shared_ptr<gnss::IAGnssRil> AGnssRil();

    /* ── the session state the vendor ops drive ── */
    void SetCallbacks(const GnssCallbackStruct &callbacks);
    void SetAgnssCallbacks(const AgnssCallbackIfaces &callbacks);
    void SetPositionConfig(int32_t ohosMode, uint32_t minIntervalMs);
    int32_t StartSession();
    int32_t StopSession();
    void Disable();

    /* ── callbacks into the OHOS HDI ──
     *
     * Every report is handed to a single worker thread of our own rather
     * than called on the binder thread it arrived on.  The stock HDI calls
     * enableGnss() and friends while holding the same mutex its report
     * callbacks take, and the MTK HAL answers setCallback()/start() with
     * synchronous callbacks — delivered inline, that is a three-way
     * deadlock across the binder.  It also keeps OHOS IPC off bionic
     * threads. */
    void Post(std::function<void()> fn);
    GnssCallbackStruct Callbacks();
    AgnssCallbackIfaces AgnssCallbacks();

    /* Called by the death recipient. */
    void OnHalDied();

private:
    GnssBridge() = default;

    void ConnectLoop();
    bool Connect();
    void ApplySession();
    void DispatchLoop();

    mutable std::mutex lock_;
    /* Serialises the HAL calls of a session change.  Held across binder
     * calls, so nothing on a binder thread may take it. */
    std::mutex sessionLock_;
    std::condition_variable connectedCv_;
    bool started_ = false;
    bool connected_ = false;
    bool unavailable_ = false;
    std::shared_ptr<gnss::IGnss> gnss_;
    std::shared_ptr<gnss::IAGnss> agnss_;
    std::shared_ptr<gnss::IAGnssRil> agnssRil_;
    ::ndk::ScopedAIBinder_DeathRecipient death_;

    GnssCallbackStruct callbacks_ {};
    AgnssCallbackIfaces agnssCallbacks_ {};
    bool sessionWanted_ = false;
    int32_t positionMode_ = 0;      /* gnss::IGnss::GnssPositionMode */
    uint32_t minIntervalMs_ = 1000;

    std::mutex queueLock_;
    std::condition_variable queueCv_;
    std::deque<std::function<void()>> queue_;
};

/* Callback objects handed to the HAL (hybris_gnss_callbacks.cpp). */
std::shared_ptr<gnss::IGnssCallback> MakeGnssCallback();
std::shared_ptr<gnss::IAGnssCallback> MakeAGnssCallback();
std::shared_ptr<gnss::IAGnssRilCallback> MakeAGnssRilCallback();

/* The AGNSS module table (hybris_gnss_agnss.cpp). */
const AgnssModuleInterface *AgnssModule();

} // namespace HybrisGnss
} // namespace OHOS

#endif // HYBRIS_GNSS_BRIDGE_H
