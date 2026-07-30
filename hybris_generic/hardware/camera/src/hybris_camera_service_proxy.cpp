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
 * Stub android.hardware.ICameraServiceProxy for the Halium container.
 *
 * Why this exists: AOSP's CameraService asks "media.camera.proxy" whether a
 * device-policy admin has disabled the camera, and CameraServiceProxyWrapper
 * returns *true* (disabled) when the service is missing.  On a normal Android
 * that service is system_server; Halium has no system_server, so every camera2
 * connectDevice() is refused with ERROR_DISABLED before it reaches the HAL.
 * Registering a permissive stub is enough to open the gate, and leaves the
 * real CameraService in charge of everything else.
 *
 * Implemented over libbinder_ndk (stable C ABI) rather than libbinder's C++
 * ABI, so libhybris only has to carry plain function pointers.  The headers
 * are not vendored in-tree, so the handful of entry points used here are
 * declared locally; they are ABI-frozen NDK API.
 */

#include "hybris_camera_service_proxy.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>

#include "hybris_dl.h"

namespace OHOS {
namespace HybrisCamera {
namespace {

// ─── libbinder_ndk ABI ──────────────────────────────────────────────────────

struct AIBinder;
struct AIBinder_Class;
struct AParcel;
struct AStatus;

using binder_status_t = int32_t;
using transaction_code_t = uint32_t;

using AIBinder_Class_onCreate = void *(*)(void *args);
using AIBinder_Class_onDestroy = void (*)(void *userData);
using AIBinder_Class_onTransact = binder_status_t (*)(AIBinder *binder, transaction_code_t code,
                                                      const AParcel *in, AParcel *out);

using FnClassDefine = AIBinder_Class *(*)(const char *, AIBinder_Class_onCreate,
                                          AIBinder_Class_onDestroy, AIBinder_Class_onTransact);
using FnBinderNew = AIBinder *(*)(const AIBinder_Class *, void *);
using FnAddService = binder_status_t (*)(AIBinder *, const char *);
using FnCheckService = AIBinder *(*)(const char *);
using FnDecStrong = void (*)(AIBinder *);
using FnStartThreadPool = void (*)();
using FnSetMaxThreads = bool (*)(uint32_t);
using FnStatusNewOk = AStatus *(*)();
using FnStatusDelete = void (*)(AStatus *);
using FnWriteStatusHeader = binder_status_t (*)(AParcel *, const AStatus *);
using FnWriteInt32 = binder_status_t (*)(AParcel *, int32_t);

FnClassDefine g_classDefine = nullptr;
FnBinderNew g_binderNew = nullptr;
FnAddService g_addService = nullptr;
FnCheckService g_checkService = nullptr;
FnDecStrong g_decStrong = nullptr;
FnStartThreadPool g_startThreadPool = nullptr;
FnSetMaxThreads g_setMaxThreads = nullptr;
FnStatusNewOk g_statusNewOk = nullptr;
FnStatusDelete g_statusDelete = nullptr;
FnWriteStatusHeader g_writeStatusHeader = nullptr;
FnWriteInt32 g_writeInt32 = nullptr;

constexpr const char *kProxyDescriptor = "android.hardware.ICameraServiceProxy";
constexpr const char *kProxyInstance = "media.camera.proxy";

/*
 * ICameraServiceProxy on Android 14, in declaration order:
 *   1 pingForUserUpdate()            oneway
 *   2 notifyCameraState(stats)       oneway
 *   3 getRotateAndCropOverride(...)  -> int
 *   4 getAutoframingOverride(...)    -> int
 *   5 isCameraDisabled(userId)       -> boolean
 * Every non-oneway method returns an int-sized value (AIDL marshals boolean as
 * int32), so one reply shape — status header OK followed by 0 — is a correct
 * answer to all of them: no rotate/crop override, no autoframing override,
 * camera not disabled.  That keeps the stub valid across interface revisions
 * as long as the two oneway methods stay first, which is why codes below
 * kFirstReplyingCode are answered without touching the reply parcel.
 */
constexpr transaction_code_t kFirstReplyingCode = 3;

void DefaultLog(const char *msg)
{
    printf("[camera-proxy] %s\n", msg);
    fflush(stdout);
}

ProxyLogFn g_log = DefaultLog;

void Logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Logf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log != nullptr) {
        g_log(buf);
    }
}

// Codes already reported once, so a chatty CameraService cannot flood the log.
uint32_t g_seenCodes = 0;

binder_status_t OnTransact(AIBinder * /*binder*/, transaction_code_t code, const AParcel * /*in*/,
                           AParcel *out)
{
    if (code < 32 && (g_seenCodes & (1u << code)) == 0) {
        g_seenCodes |= (1u << code);
        Logf("ICameraServiceProxy transaction %u (first time)", code);
    }
    if (code < kFirstReplyingCode || out == nullptr) {
        return 0; // STATUS_OK — oneway, nothing to reply with
    }
    AStatus *ok = g_statusNewOk();
    if (ok != nullptr) {
        g_writeStatusHeader(out, ok);
        g_statusDelete(ok);
    }
    g_writeInt32(out, 0);
    return 0; // STATUS_OK
}

void *OnCreate(void *args)
{
    return args;
}

void OnDestroy(void * /*userData*/) {}

bool LoadBinderNdk()
{
    if (g_classDefine != nullptr) {
        return true;
    }
    void *lib = HybrisDlopen("libbinder_ndk.so", RTLD_LAZY);
    if (lib == nullptr) {
        Logf("HybrisDlopen(libbinder_ndk.so) failed: %s", HybrisDlerror());
        return false;
    }
    struct {
        const char *name;
        void **slot;
    } syms[] = {
        { "AIBinder_Class_define", reinterpret_cast<void **>(&g_classDefine) },
        { "AIBinder_new", reinterpret_cast<void **>(&g_binderNew) },
        { "AServiceManager_addService", reinterpret_cast<void **>(&g_addService) },
        { "AServiceManager_checkService", reinterpret_cast<void **>(&g_checkService) },
        { "AIBinder_decStrong", reinterpret_cast<void **>(&g_decStrong) },
        { "ABinderProcess_startThreadPool", reinterpret_cast<void **>(&g_startThreadPool) },
        { "ABinderProcess_setThreadPoolMaxThreadCount", reinterpret_cast<void **>(&g_setMaxThreads) },
        { "AStatus_newOk", reinterpret_cast<void **>(&g_statusNewOk) },
        { "AStatus_delete", reinterpret_cast<void **>(&g_statusDelete) },
        { "AParcel_writeStatusHeader", reinterpret_cast<void **>(&g_writeStatusHeader) },
        { "AParcel_writeInt32", reinterpret_cast<void **>(&g_writeInt32) },
    };
    for (const auto &s : syms) {
        *s.slot = HybrisDlsym(lib, s.name);
        if (*s.slot == nullptr) {
            Logf("dlsym(%s) failed", s.name);
            g_classDefine = nullptr;
            return false;
        }
    }
    return true;
}

} // namespace

void SetProxyLogger(ProxyLogFn fn)
{
    g_log = (fn != nullptr) ? fn : DefaultLog;
}

bool EnsureCameraServiceProxy()
{
    static bool registered = false;
    if (registered) {
        return true;
    }
    if (!LoadBinderNdk()) {
        return false;
    }

    AIBinder *existing = g_checkService(kProxyInstance);
    if (existing != nullptr) {
        g_decStrong(existing);
        Logf("%s already registered — leaving it alone", kProxyInstance);
        registered = true;
        return true;
    }

    if (g_setMaxThreads != nullptr) {
        g_setMaxThreads(2);
    }
    g_startThreadPool();

    AIBinder_Class *clazz = g_classDefine(kProxyDescriptor, OnCreate, OnDestroy, OnTransact);
    if (clazz == nullptr) {
        Logf("AIBinder_Class_define(%s) failed", kProxyDescriptor);
        return false;
    }
    // Deliberately never released: the stub lives as long as the process.
    AIBinder *binder = g_binderNew(clazz, nullptr);
    if (binder == nullptr) {
        Logf("AIBinder_new failed");
        return false;
    }
    binder_status_t st = g_addService(binder, kProxyInstance);
    if (st != 0) {
        Logf("AServiceManager_addService(%s) failed: %d", kProxyInstance, st);
        return false;
    }
    Logf("registered stub %s (%s)", kProxyInstance, kProxyDescriptor);
    registered = true;
    return true;
}

} // namespace HybrisCamera
} // namespace OHOS
