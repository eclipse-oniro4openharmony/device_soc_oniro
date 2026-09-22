/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * oniro.androidcontainer — the "Android Apps" shell's native half.
 *
 *   const container = globalThis.requireNapi('oniro.androidcontainer')
 *
 * A SYSTEM NAPI module (/system/lib64/module/oniro/libandroidcontainer.z.so),
 * not a library bundled in the HAP, and that is the whole point.  The shell
 * has to hand the producer of its XComponent surface to the compositor's
 * session SA, which needs the inner samgr + graphic_surface APIs.  Bundled in
 * the HAP that failed twice over (the W5 attempt): BMS leaves the entry
 * module's nativeLibraryPath empty, and the app's linker namespace refuses
 * the inner libraries anyway.  A module under /system/lib64/module is opened
 * with a plain dlopen() outside the app namespace (native_module_manager) —
 * the same way @ohos.multimedia.media's AVPlayer gets a surface across.
 *
 * Any app can therefore load this module.  It grants nothing: every call ends
 * in SA 9601, which answers the shell bundle (and root) only.
 *
 * The surface itself never leaves the process as pixels: surfaceId →
 * SurfaceUtils (process-local) → IBufferProducer → one binder object.
 *
 *   getState(): number                     -100 no compositor (stack not up)
 *                                          0 booting, 1 ready, 2 frozen
 *   listApps(): Promise<{name, package}[]>
 *   getAppIcon(package): Promise<ArrayBuffer>      PNG bytes
 *   launchApp(package): Promise<number>
 *   closeApp(package): number
 *   attachWindow(surfaceId, package, width, height): number   window id, or < 0
 *   detachWindow(window): number
 *   windowAlive(window): boolean
 *   setWindowActive(window, active): void
 *   sendTouch(window, action, pointerId, x, y): void    action = OHOS PointerEvent
 *                                                       (1 cancel 2 down 3 move 4 up)
 *   sendKey(window, evdevCode): void                    press + release (BACK = 158)
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <napi/native_api.h>
#include <napi/native_node_api.h>

#include <hilog/log.h>
#include <ibuffer_producer.h>
#include <surface.h>
#include <surface_utils.h>

#include "waydroid_session.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "androidcontainer"

using namespace OHOS;
using namespace OHOS::Waydroid;

namespace {

constexpr int32_t NO_COMPOSITOR = -100;

/* The proxy is cached — touch arrives at the panel's rate and a samgr lookup
 * per event is an IPC of its own — and dropped as soon as a call fails, which
 * is how a rebuilt compositor generation is picked up. */
std::mutex g_sessionMutex;
sptr<IWaydroidSession> g_session;

sptr<IWaydroidSession> Session()
{
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    if (g_session == nullptr || g_session->AsObject() == nullptr ||
        g_session->AsObject()->IsObjectDead()) {
        g_session = WaydroidSessionProxy::Get();
    }
    return g_session;
}

void DropSession()
{
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    g_session = nullptr;
}

/* ---- argument helpers ---------------------------------------------------------- */

struct Args {
    size_t argc = 6;
    napi_value argv[6] = { nullptr };
};

Args GetArgs(napi_env env, napi_callback_info info)
{
    Args a;
    napi_get_cb_info(env, info, &a.argc, a.argv, nullptr, nullptr);
    return a;
}

std::string StringArg(napi_env env, const Args& a, size_t i)
{
    if (i >= a.argc) {
        return {};
    }
    size_t len = 0;
    if (napi_get_value_string_utf8(env, a.argv[i], nullptr, 0, &len) != napi_ok || len > 4096) {
        return {};
    }
    std::string s(len, '\0');
    napi_get_value_string_utf8(env, a.argv[i], s.data(), len + 1, &len);
    return s;
}

int32_t IntArg(napi_env env, const Args& a, size_t i)
{
    int32_t v = 0;
    if (i < a.argc) {
        /* ArkUI sizes and coordinates are doubles. */
        double d = 0;
        if (napi_get_value_double(env, a.argv[i], &d) == napi_ok) {
            v = static_cast<int32_t>(d);
        }
    }
    return v;
}

bool BoolArg(napi_env env, const Args& a, size_t i)
{
    bool v = false;
    if (i < a.argc) {
        napi_get_value_bool(env, a.argv[i], &v);
    }
    return v;
}

napi_value Int(napi_env env, int32_t v)
{
    napi_value out = nullptr;
    napi_create_int32(env, v, &out);
    return out;
}

napi_value Undefined(napi_env env)
{
    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

/* ---- synchronous calls ------------------------------------------------------------- */

napi_value GetState(napi_env env, napi_callback_info)
{
    auto session = Session();
    if (session == nullptr) {
        return Int(env, NO_COMPOSITOR);
    }
    int32_t state = session->GetState();
    if (state == SESSION_EGENERIC) {
        DropSession();
        return Int(env, NO_COMPOSITOR);
    }
    return Int(env, state);
}

napi_value AttachWindow(napi_env env, napi_callback_info info)
{
    Args a = GetArgs(env, info);
    const std::string surfaceIdText = StringArg(env, a, 0);
    const std::string package = StringArg(env, a, 1);
    const int32_t width = IntArg(env, a, 2);
    const int32_t height = IntArg(env, a, 3);

    auto session = Session();
    if (session == nullptr) {
        return Int(env, NO_COMPOSITOR);
    }
    uint64_t surfaceId = strtoull(surfaceIdText.c_str(), nullptr, 10);
    sptr<Surface> surface = SurfaceUtils::GetInstance()->GetSurface(surfaceId);
    if (surface == nullptr) {
        HILOG_ERROR(LOG_CORE, "attachWindow: no surface for id %{public}s", surfaceIdText.c_str());
        return Int(env, SESSION_EINVAL);
    }
    sptr<IBufferProducer> producer = surface->GetProducer();
    if (producer == nullptr) {
        return Int(env, SESSION_EINVAL);
    }
    int32_t window = session->AttachWindow(package, producer->AsObject(), width, height);
    HILOG_INFO(LOG_CORE, "attachWindow('%{public}s', %{public}dx%{public}d) -> %{public}d",
               package.c_str(), width, height, window);
    if (window == SESSION_EGENERIC) {
        DropSession();
    }
    return Int(env, window);
}

napi_value DetachWindow(napi_env env, napi_callback_info info)
{
    Args a = GetArgs(env, info);
    auto session = Session();
    return Int(env, session != nullptr ? session->DetachWindow(IntArg(env, a, 0)) : NO_COMPOSITOR);
}

napi_value WindowAlive(napi_env env, napi_callback_info info)
{
    Args a = GetArgs(env, info);
    auto session = Session();
    int32_t alive = session != nullptr ? session->WindowAlive(IntArg(env, a, 0)) : 0;
    if (alive < 0) {
        DropSession();
    }
    napi_value out = nullptr;
    napi_get_boolean(env, alive == 1, &out);
    return out;
}

napi_value SetWindowActive(napi_env env, napi_callback_info info)
{
    Args a = GetArgs(env, info);
    auto session = Session();
    if (session != nullptr) {
        session->SetWindowActive(IntArg(env, a, 0), BoolArg(env, a, 1));
    }
    return Undefined(env);
}

napi_value SendTouch(napi_env env, napi_callback_info info)
{
    Args a = GetArgs(env, info);
    auto session = Session();
    if (session != nullptr &&
        session->WindowTouch(IntArg(env, a, 0), IntArg(env, a, 1), IntArg(env, a, 2),
                             IntArg(env, a, 3), IntArg(env, a, 4)) != SESSION_OK) {
        DropSession();
    }
    return Undefined(env);
}

napi_value SendKey(napi_env env, napi_callback_info info)
{
    Args a = GetArgs(env, info);
    auto session = Session();
    if (session != nullptr) {
        const int32_t window = IntArg(env, a, 0);
        const int32_t code = IntArg(env, a, 1);
        session->WindowKey(window, code, true);
        session->WindowKey(window, code, false);
    }
    return Undefined(env);
}

napi_value CloseApp(napi_env env, napi_callback_info info)
{
    Args a = GetArgs(env, info);
    auto session = Session();
    return Int(env, session != nullptr ? session->CloseApp(StringArg(env, a, 0)) : NO_COMPOSITOR);
}

/* ---- promises: calls that reach into Android ---------------------------------------- */

struct AsyncCall {
    enum Kind { LIST_APPS, GET_ICON, LAUNCH } kind = LIST_APPS;
    std::string package;
    int32_t result = SESSION_EGENERIC;
    std::vector<SessionApp> apps;
    std::vector<uint8_t> bytes;
    napi_deferred deferred = nullptr;
    napi_async_work work = nullptr;
};

void ExecuteAsync(napi_env, void* data)
{
    auto* call = static_cast<AsyncCall*>(data);
    auto session = Session();
    if (session == nullptr) {
        call->result = NO_COMPOSITOR;
        return;
    }
    switch (call->kind) {
        case AsyncCall::LIST_APPS:
            call->result = session->ListApps(call->apps);
            break;
        case AsyncCall::GET_ICON:
            call->result = session->GetAppIcon(call->package, call->bytes);
            break;
        case AsyncCall::LAUNCH:
            call->result = session->LaunchApp(call->package);
            break;
    }
    if (call->result == SESSION_EGENERIC) {
        DropSession();
    }
}

void CompleteAsync(napi_env env, napi_status, void* data)
{
    std::unique_ptr<AsyncCall> call(static_cast<AsyncCall*>(data));
    napi_value value = nullptr;
    bool ok = call->result == SESSION_OK;
    if (call->kind == AsyncCall::LAUNCH) {
        value = Int(env, call->result);
        ok = true;                      /* the code is the answer */
    } else if (!ok) {
        /* Reject with the code: the caller tells "frozen" from "booting". */
        napi_value code = Int(env, call->result);
        napi_value msg = nullptr;
        napi_create_string_utf8(env, "android container call failed", NAPI_AUTO_LENGTH, &msg);
        napi_create_error(env, nullptr, msg, &value);
        napi_set_named_property(env, value, "code", code);
    } else if (call->kind == AsyncCall::LIST_APPS) {
        napi_create_array_with_length(env, call->apps.size(), &value);
        for (size_t i = 0; i < call->apps.size(); i++) {
            napi_value app = nullptr;
            napi_value name = nullptr;
            napi_value package = nullptr;
            napi_create_object(env, &app);
            napi_create_string_utf8(env, call->apps[i].name.c_str(), NAPI_AUTO_LENGTH, &name);
            napi_create_string_utf8(env, call->apps[i].package.c_str(), NAPI_AUTO_LENGTH, &package);
            napi_set_named_property(env, app, "name", name);
            napi_set_named_property(env, app, "package", package);
            napi_set_element(env, value, static_cast<uint32_t>(i), app);
        }
    } else {
        void* buffer = nullptr;
        napi_create_arraybuffer(env, call->bytes.size(), &buffer, &value);
        if (buffer != nullptr && !call->bytes.empty()) {
            memcpy(buffer, call->bytes.data(), call->bytes.size());
        }
    }
    if (ok) {
        napi_resolve_deferred(env, call->deferred, value);
    } else {
        napi_reject_deferred(env, call->deferred, value);
    }
    napi_delete_async_work(env, call->work);
}

napi_value StartAsync(napi_env env, AsyncCall::Kind kind, const std::string& package)
{
    auto* call = new AsyncCall();
    call->kind = kind;
    call->package = package;
    napi_value promise = nullptr;
    napi_create_promise(env, &call->deferred, &promise);
    napi_value name = nullptr;
    napi_create_string_utf8(env, "androidcontainer", NAPI_AUTO_LENGTH, &name);
    napi_create_async_work(env, nullptr, name, ExecuteAsync, CompleteAsync, call, &call->work);
    napi_queue_async_work(env, call->work);
    return promise;
}

napi_value ListApps(napi_env env, napi_callback_info)
{
    return StartAsync(env, AsyncCall::LIST_APPS, {});
}

napi_value GetAppIcon(napi_env env, napi_callback_info info)
{
    Args a = GetArgs(env, info);
    return StartAsync(env, AsyncCall::GET_ICON, StringArg(env, a, 0));
}

napi_value LaunchApp(napi_env env, napi_callback_info info)
{
    Args a = GetArgs(env, info);
    return StartAsync(env, AsyncCall::LAUNCH, StringArg(env, a, 0));
}

napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        { "getState", nullptr, GetState, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "listApps", nullptr, ListApps, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getAppIcon", nullptr, GetAppIcon, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "launchApp", nullptr, LaunchApp, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "closeApp", nullptr, CloseApp, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "attachWindow", nullptr, AttachWindow, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "detachWindow", nullptr, DetachWindow, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "windowAlive", nullptr, WindowAlive, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setWindowActive", nullptr, SetWindowActive, nullptr, nullptr, nullptr, napi_default,
          nullptr },
        { "sendTouch", nullptr, SendTouch, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "sendKey", nullptr, SendKey, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace

static napi_module g_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "oniro.androidcontainer",
    .nm_priv = nullptr,
    .reserved = { nullptr },
};

extern "C" __attribute__((constructor)) void RegisterAndroidContainerModule()
{
    napi_module_register(&g_module);
}
