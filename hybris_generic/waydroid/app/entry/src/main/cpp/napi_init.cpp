/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Native module for the "Android Apps" front-end (W5).
 *
 * The ArkUI page hosts a fullscreen XComponent (SURFACE type, libraryname
 * "androidapps").  ACE calls us back with the component's OHNativeWindow;
 * we pull its producer and hand it to the compositor's session SA, so the
 * Android container composites straight into this app's window (same
 * zero-copy frame path as the W3 self-drawing node, now owned by a real
 * OHOS window that respects focus/rotation/recents).  XComponent touch is
 * forwarded to the container; the ability's fore/background drives the
 * cgroup freezer.
 *
 * In-tree build: this links the inner graphic_surface + samgr APIs (an
 * NDK-only HAP could not reach samgr), which is why the app is a gn
 * ohos_hap with this as a bundled ohos_shared_library.
 */

#include <cstdint>

#include <napi/native_api.h>
#include <native/native_interface_xcomponent.h>

#include <external_window.h>     /* NDK: OH_NativeWindow_GetSurfaceId */
#include <surface.h>
#include <surface_utils.h>
#include <ibuffer_producer.h>
#include <hilog/log.h>

#include "waydroid_session.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "androidapps"

using namespace OHOS;
using namespace OHOS::Waydroid;

namespace {

/* OHOS PointerEvent action codes the compositor expects. */
constexpr int32_t ACTION_CANCEL = 1;
constexpr int32_t ACTION_DOWN = 2;
constexpr int32_t ACTION_MOVE = 3;
constexpr int32_t ACTION_UP = 4;

sptr<IWaydroidSession> Session()
{
    static sptr<IWaydroidSession> s = WaydroidSessionProxy::Get();
    if (s == nullptr) {
        s = WaydroidSessionProxy::Get();   /* retry: compositor may lag */
    }
    return s;
}

void OnSurfaceCreated(OH_NativeXComponent* /*component*/, void* window)
{
    /* The XComponent surface lives in THIS process, so resolve its
     * producer locally: NDK surfaceId -> SurfaceUtils (no internal
     * NativeWindow struct needed), then hand only the producer object
     * across to the compositor. */
    uint64_t surfaceId = 0;
    if (OH_NativeWindow_GetSurfaceId(reinterpret_cast<OHNativeWindow*>(window),
                                     &surfaceId) != 0) {
        HILOG_ERROR(LOG_CORE, "OnSurfaceCreated: GetSurfaceId failed");
        return;
    }
    sptr<Surface> surface = SurfaceUtils::GetInstance()->GetSurface(surfaceId);
    if (surface == nullptr) {
        HILOG_ERROR(LOG_CORE, "OnSurfaceCreated: no surface for id %{public}llu",
                    static_cast<unsigned long long>(surfaceId));
        return;
    }
    sptr<IBufferProducer> producer = surface->GetProducer();
    auto session = Session();
    if (producer == nullptr || session == nullptr) {
        HILOG_ERROR(LOG_CORE, "OnSurfaceCreated: producer=%{public}d session=%{public}d",
                    producer != nullptr, session != nullptr);
        return;
    }
    /* The window is showing: thaw the container, then hand it our surface
     * so it composites into this app's XComponent. */
    session->SetForeground(true);
    int32_t r = session->SetOutputSurface(producer->AsObject());
    HILOG_INFO(LOG_CORE, "handed surface to compositor -> %{public}d", r);
}

void OnSurfaceDestroyed(OH_NativeXComponent* /*component*/, void* /*window*/)
{
    auto session = Session();
    if (session != nullptr) {
        /* Window torn down: revert output and freeze the container so a
         * hidden Android session costs ~0 CPU. */
        session->ClearOutputSurface();
        session->SetForeground(false);
        HILOG_INFO(LOG_CORE, "surface destroyed -> reverted output + froze container");
    }
}

void DispatchTouchEvent(OH_NativeXComponent* component, void* window)
{
    OH_NativeXComponent_TouchEvent touch;
    if (OH_NativeXComponent_GetTouchEvent(component, window, &touch) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    auto session = Session();
    if (session == nullptr) {
        return;
    }
    int32_t action;
    switch (touch.type) {
        case OH_NATIVEXCOMPONENT_DOWN: action = ACTION_DOWN; break;
        case OH_NATIVEXCOMPONENT_UP:   action = ACTION_UP; break;
        case OH_NATIVEXCOMPONENT_MOVE: action = ACTION_MOVE; break;
        case OH_NATIVEXCOMPONENT_CANCEL: action = ACTION_CANCEL; break;
        default: return;
    }
    /* Fullscreen XComponent: component-local coords == panel coords. */
    for (uint32_t i = 0; i < touch.numPoints && i < OH_MAX_TOUCH_POINTS_NUMBER; i++) {
        const auto& p = touch.touchPoints[i];
        int32_t a = action;
        if (touch.numPoints > 1) {
            /* Per-point phase for multitouch. */
            if (p.type == OH_NATIVEXCOMPONENT_DOWN) a = ACTION_DOWN;
            else if (p.type == OH_NATIVEXCOMPONENT_UP) a = ACTION_UP;
            else if (p.type == OH_NATIVEXCOMPONENT_MOVE) a = ACTION_MOVE;
            else if (p.type == OH_NATIVEXCOMPONENT_CANCEL) a = ACTION_CANCEL;
        }
        session->InjectTouch(a, p.id, static_cast<int32_t>(p.x),
                             static_cast<int32_t>(p.y));
    }
}

OH_NativeXComponent_Callback g_xcomponentCallback = {
    .OnSurfaceCreated = OnSurfaceCreated,
    .OnSurfaceChanged = nullptr,
    .OnSurfaceDestroyed = OnSurfaceDestroyed,
    .DispatchTouchEvent = DispatchTouchEvent,
};

/* Grab the OH_NativeXComponent from the exports object ACE passes us and
 * register our callbacks. */
void BindXComponent(napi_env env, napi_value exports)
{
    napi_value xcompObj = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ,
                               &xcompObj) != napi_ok || xcompObj == nullptr) {
        HILOG_ERROR(LOG_CORE, "no NativeXComponent object in exports");
        return;
    }
    OH_NativeXComponent* nativeXComponent = nullptr;
    if (napi_unwrap(env, xcompObj,
                    reinterpret_cast<void**>(&nativeXComponent)) != napi_ok ||
        nativeXComponent == nullptr) {
        HILOG_ERROR(LOG_CORE, "unwrap NativeXComponent failed");
        return;
    }
    OH_NativeXComponent_RegisterCallback(nativeXComponent, &g_xcomponentCallback);
    HILOG_INFO(LOG_CORE, "XComponent callbacks registered");
}

/* setForeground(fg: boolean): number — the ability calls this on
 * fore/background to thaw/freeze the container. */
napi_value SetForeground(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool fg = false;
    if (argc >= 1) {
        napi_get_value_bool(env, argv[0], &fg);
    }
    int32_t r = -1;
    auto session = Session();
    if (session != nullptr) {
        r = session->SetForeground(fg);
    }
    napi_value out;
    napi_create_int32(env, r, &out);
    return out;
}

napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        { "setForeground", nullptr, SetForeground, nullptr, nullptr, nullptr,
          napi_default, nullptr },
    };
    napi_define_properties(env, exports,
                           sizeof(desc) / sizeof(desc[0]), desc);
    BindXComponent(env, exports);
    return exports;
}

} // namespace

static napi_module g_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "androidapps",
    .nm_priv = nullptr,
    .reserved = { nullptr },
};

extern "C" __attribute__((constructor)) void RegisterAndroidAppsModule()
{
    napi_module_register(&g_module);
}
