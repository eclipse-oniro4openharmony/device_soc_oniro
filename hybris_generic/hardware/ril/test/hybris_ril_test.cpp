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
 * R1 harness — prove an OHOS process can drive the Halium container's AIDL
 * IRadio v2 HAL end to end, independently of any OHOS telephony plumbing.
 *
 * Not installed into the image.  Build it, push it, run it:
 *
 *   ninja -w dupbuild=warn device_hybris_generic/.../hybris_ril_test
 *   hdc file send .../hybris_ril_test /data/hybris_ril_test
 *   hdc shell "chmod +x /data/hybris_ril_test && /data/hybris_ril_test"
 *
 * What it exercises, in order:
 *   1. libbinder_ndk through libhybris (the trampoline table)
 *   2. the container's servicemanager over /dev/binder
 *   3. an outgoing vintf-stable AIDL call     (IRadioModem::getBasebandVersion)
 *   4. an incoming callback on a bionic thread (IRadioModemResponse)
 *   5. an unsolicited indication               (IRadioSimIndication, etc.)
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <aidl/android/hardware/radio/config/BnRadioConfigIndication.h>
#include <aidl/android/hardware/radio/config/BnRadioConfigResponse.h>
#include <aidl/android/hardware/radio/config/IRadioConfig.h>
#include <aidl/android/hardware/radio/modem/BnRadioModemIndication.h>
#include <aidl/android/hardware/radio/modem/BnRadioModemResponse.h>
#include <aidl/android/hardware/radio/modem/IRadioModem.h>
#include <aidl/android/hardware/radio/network/BnRadioNetworkIndication.h>
#include <aidl/android/hardware/radio/network/BnRadioNetworkResponse.h>
#include <aidl/android/hardware/radio/network/IRadioNetwork.h>
#include <aidl/android/hardware/radio/sim/BnRadioSimIndication.h>
#include <aidl/android/hardware/radio/sim/BnRadioSimResponse.h>
#include <aidl/android/hardware/radio/sim/IRadioSim.h>

#include "hybris_binder_ndk.h"

using namespace std::chrono_literals;
namespace radio = aidl::android::hardware::radio;

namespace {

std::mutex g_lock;
std::condition_variable g_cv;
std::atomic<int> g_responses{0};
std::atomic<int> g_indications{0};

void Say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vprintf(fmt, ap);
    va_end(ap);
    (void)fflush(stdout);
}

void Note(const radio::RadioResponseInfo &info, const char *what)
{
    Say("  <- %-28s serial=%d type=%d error=%d\n", what, info.serial,
        static_cast<int>(info.type), static_cast<int>(info.error));
    g_responses++;
    g_cv.notify_all();
}

void Ind(const char *what)
{
    Say("  ~~ indication: %s\n", what);
    g_indications++;
}

/* ---- modem ----------------------------------------------------------- */

class ModemResponse : public radio::modem::IRadioModemResponseDefault {
public:
    ::ndk::ScopedAStatus getBasebandVersionResponse(const radio::RadioResponseInfo &info,
                                                    const std::string &version) override
    {
        Note(info, "getBasebandVersionResponse");
        Say("     baseband: '%s'\n", version.c_str());
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getImeiResponse(
        const radio::RadioResponseInfo &info,
        const std::optional<radio::modem::ImeiInfo> &imeiInfo) override
    {
        Note(info, "getImeiResponse");
        if (imeiInfo.has_value()) {
            Say("     imei: '%s' svn '%s' type %d\n", imeiInfo->imei.c_str(),
                imeiInfo->svn.c_str(), static_cast<int>(imeiInfo->type));
        } else {
            Say("     imei: <null>\n");
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus setRadioPowerResponse(const radio::RadioResponseInfo &info) override
    {
        Note(info, "setRadioPowerResponse");
        return ::ndk::ScopedAStatus::ok();
    }
};

class ModemIndication : public radio::modem::IRadioModemIndicationDefault {
public:
    ::ndk::ScopedAStatus radioStateChanged(radio::RadioIndicationType,
                                           radio::modem::RadioState radioState) override
    {
        Say("  ~~ indication: radioStateChanged -> %d\n", static_cast<int>(radioState));
        g_indications++;
        return ::ndk::ScopedAStatus::ok();
    }
    ::ndk::ScopedAStatus rilConnected(radio::RadioIndicationType) override
    {
        Ind("rilConnected");
        return ::ndk::ScopedAStatus::ok();
    }
    ::ndk::ScopedAStatus modemReset(radio::RadioIndicationType, const std::string &reason) override
    {
        Say("  ~~ indication: modemReset '%s'\n", reason.c_str());
        g_indications++;
        return ::ndk::ScopedAStatus::ok();
    }
};

/* ---- sim ------------------------------------------------------------- */

const char *CardStateName(int s)
{
    switch (s) {
        case 0: return "ABSENT";
        case 1: return "PRESENT";
        case 2: return "ERROR";
        case 3: return "RESTRICTED";
        default: return "?";
    }
}

class SimResponse : public radio::sim::IRadioSimResponseDefault {
public:
    ::ndk::ScopedAStatus getIccCardStatusResponse(const radio::RadioResponseInfo &info,
                                                  const radio::sim::CardStatus &card) override
    {
        Note(info, "getIccCardStatusResponse");
        Say("     cardState=%d (%s) apps=%zu iccid='%s' atr='%s'\n", card.cardState,
            CardStateName(card.cardState), card.applications.size(), card.iccid.c_str(),
            card.atr.c_str());
        for (size_t i = 0; i < card.applications.size(); i++) {
            const auto &a = card.applications[i];
            Say("       app[%zu] type=%d state=%d aid='%s' label='%s' pin1=%d\n", i, a.appType,
                a.appState, a.aidPtr.c_str(), a.appLabelPtr.c_str(), static_cast<int>(a.pin1));
        }
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getImsiForAppResponse(const radio::RadioResponseInfo &info,
                                               const std::string &imsi) override
    {
        Note(info, "getImsiForAppResponse");
        /* An IMSI is PII; the MCC/MNC prefix is what bring-up actually needs. */
        Say("     imsi: '%.5s...' (%zu digits)\n", imsi.c_str(), imsi.size());
        return ::ndk::ScopedAStatus::ok();
    }
};

class SimIndication : public radio::sim::IRadioSimIndicationDefault {
public:
    ::ndk::ScopedAStatus simStatusChanged(radio::RadioIndicationType) override
    {
        Ind("simStatusChanged");
        return ::ndk::ScopedAStatus::ok();
    }
};

/* ---- network --------------------------------------------------------- */

class NetworkResponse : public radio::network::IRadioNetworkResponseDefault {
public:
    ::ndk::ScopedAStatus getSignalStrengthResponse(
        const radio::RadioResponseInfo &info,
        const radio::network::SignalStrength &sig) override
    {
        Note(info, "getSignalStrengthResponse");
        Say("     lte rsrp=%d rssnr=%d  gsm rssi=%d  nr ssRsrp=%d\n", sig.lte.rsrp, sig.lte.rssnr,
            sig.gsm.signalStrength, sig.nr.ssRsrp);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getOperatorResponse(const radio::RadioResponseInfo &info,
                                             const std::string &longName,
                                             const std::string &shortName,
                                             const std::string &numeric) override
    {
        Note(info, "getOperatorResponse");
        Say("     operator: '%s' / '%s' / %s\n", longName.c_str(), shortName.c_str(),
            numeric.c_str());
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getVoiceRegistrationStateResponse(
        const radio::RadioResponseInfo &info,
        const radio::network::RegStateResult &reg) override
    {
        Note(info, "getVoiceRegistrationStateResponse");
        Say("     regState=%d rat=%d reasonForDenial=%d\n", static_cast<int>(reg.regState),
            static_cast<int>(reg.rat), reg.reasonForDenial);
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus getDataRegistrationStateResponse(
        const radio::RadioResponseInfo &info,
        const radio::network::RegStateResult &reg) override
    {
        Note(info, "getDataRegistrationStateResponse");
        Say("     regState=%d rat=%d\n", static_cast<int>(reg.regState),
            static_cast<int>(reg.rat));
        return ::ndk::ScopedAStatus::ok();
    }
};

class NetworkIndication : public radio::network::IRadioNetworkIndicationDefault {
public:
    ::ndk::ScopedAStatus networkStateChanged(radio::RadioIndicationType) override
    {
        Ind("networkStateChanged");
        return ::ndk::ScopedAStatus::ok();
    }
    ::ndk::ScopedAStatus currentSignalStrength(radio::RadioIndicationType,
                                               const radio::network::SignalStrength &sig) override
    {
        Say("  ~~ indication: currentSignalStrength lte.rsrp=%d gsm.rssi=%d\n", sig.lte.rsrp,
            sig.gsm.signalStrength);
        g_indications++;
        return ::ndk::ScopedAStatus::ok();
    }
};

/* ---------------------------------------------------------------------- */

template <typename T>
std::shared_ptr<T> GetService(const std::string &instance)
{
    Say("waiting for %s ...\n", instance.c_str());
    ::ndk::SpAIBinder binder(AServiceManager_waitForService(instance.c_str()));
    if (binder.get() == nullptr) {
        Say("  !! not available\n");
        return nullptr;
    }
    auto svc = T::fromBinder(binder);
    if (svc == nullptr) {
        Say("  !! fromBinder() returned null (descriptor mismatch?)\n");
        return nullptr;
    }
    int32_t ver = -1;
    auto st = svc->getInterfaceVersion(&ver);
    Say("  ok — interface version %d (%s)\n", ver, st.isOk() ? "ok" : st.getDescription().c_str());
    return svc;
}

} // namespace

int main(int argc, char **argv)
{
    const char *slot = (argc > 1) ? argv[1] : "slot1";
    Say("== hybris_ril_test (%s) ==\n", slot);

    if (!OHOS::HybrisRil::BinderNdkInit()) {
        Say("!! BinderNdkInit failed — is the androidd container up?\n");
        return 1;
    }
    Say("libbinder_ndk bound through libhybris\n");

    /* Callbacks arrive on binder threads; two is plenty for a probe. */
    OHOS::HybrisRil::BinderNdkStartThreadPool(2);

    auto config = GetService<radio::config::IRadioConfig>(
        std::string(radio::config::IRadioConfig::descriptor) + "/default");
    auto modem = GetService<radio::modem::IRadioModem>(
        std::string(radio::modem::IRadioModem::descriptor) + "/" + slot);
    auto sim = GetService<radio::sim::IRadioSim>(
        std::string(radio::sim::IRadioSim::descriptor) + "/" + slot);
    auto network = GetService<radio::network::IRadioNetwork>(
        std::string(radio::network::IRadioNetwork::descriptor) + "/" + slot);
    if (modem == nullptr || sim == nullptr || network == nullptr) {
        Say("!! required services missing — is vendor.mtk.md1.status=ready?\n");
        return 1;
    }

    /* Serving a binder object back to rild: the Delegator wraps our partial
     * implementation in the generated Bn skeleton, which marks itself vintf
     * stable — mandatory, rild rejects non-vintf binders. */
    auto modemResp = ::ndk::SharedRefBase::make<radio::modem::IRadioModemResponseDelegator>(
        ::ndk::SharedRefBase::make<ModemResponse>());
    auto modemInd = ::ndk::SharedRefBase::make<radio::modem::IRadioModemIndicationDelegator>(
        ::ndk::SharedRefBase::make<ModemIndication>());
    auto simResp = ::ndk::SharedRefBase::make<radio::sim::IRadioSimResponseDelegator>(
        ::ndk::SharedRefBase::make<SimResponse>());
    auto simInd = ::ndk::SharedRefBase::make<radio::sim::IRadioSimIndicationDelegator>(
        ::ndk::SharedRefBase::make<SimIndication>());
    auto netResp = ::ndk::SharedRefBase::make<radio::network::IRadioNetworkResponseDelegator>(
        ::ndk::SharedRefBase::make<NetworkResponse>());
    auto netInd = ::ndk::SharedRefBase::make<radio::network::IRadioNetworkIndicationDelegator>(
        ::ndk::SharedRefBase::make<NetworkIndication>());

    Say("\nsetResponseFunctions ...\n");
    auto st = modem->setResponseFunctions(modemResp, modemInd);
    Say("  modem:   %s\n", st.isOk() ? "ok" : st.getDescription().c_str());
    st = sim->setResponseFunctions(simResp, simInd);
    Say("  sim:     %s\n", st.isOk() ? "ok" : st.getDescription().c_str());
    st = network->setResponseFunctions(netResp, netInd);
    Say("  network: %s\n", st.isOk() ? "ok" : st.getDescription().c_str());

    Say("\nrequests ...\n");
    int serial = 100;
    modem->getBasebandVersion(serial++);
    modem->getImei(serial++);
    sim->getIccCardStatus(serial++);
    /* Radio power is off until someone asks for it — nothing else in this
     * image ever does, which is why registration never starts today. */
    modem->setRadioPower(serial++, true, false, false);

    std::this_thread::sleep_for(3s);
    Say("\nafter radio power on ...\n");
    network->getSignalStrength(serial++);
    network->getOperator(serial++);
    network->getVoiceRegistrationState(serial++);
    network->getDataRegistrationState(serial++);
    sim->getIccCardStatus(serial++);

    Say("\nlistening for indications (30 s) ...\n");
    for (int i = 0; i < 30; i++) {
        std::this_thread::sleep_for(1s);
        if (i == 14) {
            Say("\n-- 15 s mark: re-reading registration --\n");
            network->getVoiceRegistrationState(serial++);
            network->getDataRegistrationState(serial++);
            network->getOperator(serial++);
        }
    }

    Say("\n== done: %d responses, %d indications ==\n", g_responses.load(),
        g_indications.load());
    return (g_responses.load() > 0) ? 0 : 2;
}
