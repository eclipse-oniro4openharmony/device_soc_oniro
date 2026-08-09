/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * waydroid_compositor — the OHOS side of the Waydroid display bridge.
 *
 * An init-started native service (root, sandbox:0): it must NOT be an
 * app process, because the gralloc import it does per frame needs the
 * nested /android/vendor mount that appspawn'd sandboxes cannot see.
 *
 * Stage 1 (this file today): output is a self-drawing RSSurfaceNode, so
 * container pixels reach the panel with no ArkUI app in the picture.
 * Stage 2 swaps in an XComponent producer over binder — same frame path.
 */

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include <hilog/log.h>
#include <parameter.h>
#include <transaction/rs_interfaces.h>

#include <hybris/gralloc/gralloc.h>

/* W4 touch: OHOS MMI interceptor → wl_touch (see TouchFeeder below) */
#include <accesstoken_kit.h>
#include <input_manager.h>
#include <nativetoken_kit.h>
#include <token_setproc.h>

#include "waydroid_session.h"
#include "wl_server.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "waydroid_compositor"

using namespace OHOS;
using namespace OHOS::Waydroid;

namespace {

Server* g_server = nullptr;

void OnSignal(int sig)
{
    HILOG_INFO(LOG_CORE, "signal %{public}d — stopping", sig);
    if (g_server != nullptr) {
        g_server->Stop();
    }
}

/* Panel geometry from RS; falls back to the ansuz panel if the query
 * fails (which happens only if RS is not up yet — the service is
 * ordered after it, so treat it as a soft error). */
void QueryDisplay(ServerConfig& cfg)
{
    auto& rs = Rosen::RSInterfaces::GetInstance();
    Rosen::ScreenId screenId = rs.GetDefaultScreenId();
    if (screenId == Rosen::INVALID_SCREEN_ID) {
        HILOG_WARN(LOG_CORE, "no default screen — using %{public}dx%{public}d",
                   cfg.width, cfg.height);
        return;
    }
    Rosen::RSScreenModeInfo mode = rs.GetScreenActiveMode(screenId);
    if (mode.GetScreenWidth() > 0 && mode.GetScreenHeight() > 0) {
        cfg.screenId = screenId;
        cfg.width  = mode.GetScreenWidth();
        cfg.height = mode.GetScreenHeight();
        if (mode.GetScreenRefreshRate() > 0) {
            cfg.refreshMHz = mode.GetScreenRefreshRate() * 1000;
        }
    }
    HILOG_INFO(LOG_CORE, "screen %{public}llu: %{public}dx%{public}d @%{public}d mHz",
               static_cast<unsigned long long>(cfg.screenId),
               cfg.width, cfg.height, cfg.refreshMHz);
}

/*
 * W4 stage-1 touch: a global MMI interceptor while the container owns
 * the panel.  deviceTags = TOUCH only, so key events (power/volume)
 * are never intercepted — the server skips interceptors whose tags
 * lack KEYBOARD (event_interceptor_handler.cpp).  The product path
 * (W5) replaces this with XComponent-scoped events over IPC; this one
 * stays as the headless debug rig, killable at runtime:
 *
 *   param set waydroid.input.grab 0   # touch back to OHOS
 *   param set waydroid.input.grab 1   # touch to Android
 */
constexpr const char* kGrabParam = "waydroid.input.grab";

class TouchFeeder : public MMI::IInputEventConsumer {
public:
    explicit TouchFeeder(Server* server) : server_(server) {}

    void OnInputEvent(std::shared_ptr<MMI::KeyEvent>) const override {}
    void OnInputEvent(std::shared_ptr<MMI::AxisEvent>) const override {}

    void OnInputEvent(std::shared_ptr<MMI::PointerEvent> event) const override
    {
        if (event == nullptr || server_ == nullptr) {
            return;
        }
        std::vector<TouchOp> ops;
        const int32_t pid = event->GetPointerId();
        MMI::PointerEvent::PointerItem item;
        switch (event->GetPointerAction()) {
            case MMI::PointerEvent::POINTER_ACTION_DOWN:
                if (event->GetPointerItem(pid, item)) {
                    ops.push_back({ TouchOp::Down, pid,
                                    item.GetDisplayX(), item.GetDisplayY() });
                }
                break;
            case MMI::PointerEvent::POINTER_ACTION_UP:
                ops.push_back({ TouchOp::Up, pid, 0, 0 });
                break;
            case MMI::PointerEvent::POINTER_ACTION_MOVE:
                for (int32_t id : event->GetPointerIds()) {
                    if (event->GetPointerItem(id, item) && item.IsPressed()) {
                        ops.push_back({ TouchOp::Motion, id,
                                        item.GetDisplayX(), item.GetDisplayY() });
                    }
                }
                break;
            case MMI::PointerEvent::POINTER_ACTION_CANCEL:
                ops.push_back({ TouchOp::Cancel, 0, 0, 0 });
                break;
            default:
                return;
        }
        if (!ops.empty()) {
            ops.push_back({ TouchOp::Frame, 0, 0, 0 });
            HILOG_DEBUG(LOG_CORE, "MMI touch action=%{public}d -> %{public}zu ops",
                        event->GetPointerAction(), ops.size());
            server_->InjectTouchOps(ops);
        }
    }

private:
    Server* server_;
};

/* The interceptor needs ohos.permission.INTERCEPT_INPUT_EVENT; native
 * services get it by minting their own native token (same recipe as
 * the camera HDI test). */
void GrantInputPermission()
{
    const char* perms[] = { "ohos.permission.INTERCEPT_INPUT_EVENT" };
    NativeTokenInfoParams params = {
        .dcapsNum = 0,
        .permsNum = 1,
        .aclsNum = 0,
        .dcaps = nullptr,
        .perms = perms,
        .acls = nullptr,
        .processName = "waydroid_compositor",
        .aplStr = "system_core",
    };
    uint64_t tokenId = GetAccessTokenId(&params);
    if (tokenId == 0) {
        HILOG_ERROR(LOG_CORE, "GetAccessTokenId failed — no touch grab");
        return;
    }
    SetSelfTokenID(tokenId);
    Security::AccessToken::AccessTokenKit::ReloadNativeTokenInfo();
}

/* Add/remove the interceptor as waydroid.input.grab flips; state is
 * guarded by a plain mutex (flips are rare, callbacks arrive on the
 * param-watch thread). */
class GrabController {
public:
    GrabController(Server* server) : feeder_(std::make_shared<TouchFeeder>(server)) {}

    void Apply(bool grab)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (grab && interceptorId_ < 0) {
            interceptorId_ = MMI::InputManager::GetInstance()->AddInterceptor(
                feeder_, MMI::DEFUALT_INTERCEPTOR_PRIORITY,
                MMI::CapabilityToTags(MMI::INPUT_DEV_CAP_TOUCH));
            HILOG_INFO(LOG_CORE, "touch grab on: interceptor %{public}d",
                       interceptorId_);
        } else if (!grab && interceptorId_ >= 0) {
            MMI::InputManager::GetInstance()->RemoveInterceptor(interceptorId_);
            interceptorId_ = -1;
            HILOG_INFO(LOG_CORE, "touch grab off");
        }
    }

    void ApplyFromParam()
    {
        char value[8] = { 0 };
        int n = GetParameter(kGrabParam, "1", value, sizeof value);
        Apply(n < 0 || strcmp(value, "0") != 0);
    }

private:
    std::mutex mutex_;
    std::shared_ptr<TouchFeeder> feeder_;
    int32_t interceptorId_ = -1;
};

GrabController* g_grab = nullptr;

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    signal(SIGTERM, OnSignal);
    signal(SIGINT, OnSignal);
    signal(SIGPIPE, SIG_IGN);   /* a dying wayland client must not kill us */

    /* server_wlegl imports every incoming handle through the gralloc
     * mapper, which must be loaded before the first client connects —
     * without this it retains/imports into a null module and the
     * client's create_buffer fails with "invalid native handle". */
    hybris_gralloc_initialize(0 /* no framebuffer */);

    ServerConfig cfg;
    QueryDisplay(cfg);

    Server server;
    g_server = &server;
    if (!server.Init(cfg)) {
        HILOG_ERROR(LOG_CORE, "server init failed");
        return 1;
    }

    if (!server.Output().AttachSelfDrawingNode(cfg.screenId, cfg.width, cfg.height)) {
        HILOG_ERROR(LOG_CORE, "output attach failed");
        return 1;
    }

    /* W4 touch: grab the touchscreen for the container (touch only —
     * keys stay with OHOS), toggled by waydroid.input.grab. */
    GrantInputPermission();
    GrabController grab(&server);
    g_grab = &grab;
    grab.ApplyFromParam();
    WatchParameter(kGrabParam, [](const char*, const char*, void*) {
        if (g_grab != nullptr) {
            g_grab->ApplyFromParam();
        }
    }, nullptr);

    /* W5: publish the session SA so the "Android Apps" front-end can hand
     * over its XComponent surface, forward touch, and drive lifecycle. */
    WaydroidSessionStub::Publish(&server);

    /* Tell waydroidd the socket exists; it gates container start on this
     * (the hwc's own 5 s retry loop would absorb the race, but the
     * handshake keeps the logs clean — same shape as androidd's
     * android.composer.ready). */
    SetParameter("waydroid.compositor.ready", "1");

    HILOG_INFO(LOG_CORE, "entering event loop");
    server.Run();

    WaydroidSessionStub::Withdraw();
    grab.Apply(false);
    g_grab = nullptr;
    SetParameter("waydroid.compositor.ready", "0");
    HILOG_INFO(LOG_CORE, "exiting");
    return 0;
}
