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

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include <hilog/log.h>
#include <parameter.h>
#include <transaction/rs_interfaces.h>

#include <hybris/gralloc/gralloc.h>

/* W4 touch: OHOS MMI interceptor → wl_touch (see TouchFeeder below) */
#include <accesstoken_kit.h>
#include <input_manager.h>
#include <key_event.h>
#include <key_option.h>
#include <nativetoken_kit.h>
#include <token_setproc.h>


/* W5 lifecycle: the pure-ArkUI launcher publishes SHOW/HIDE common events
 * on fore/background (it cannot reach the session SA from its sandbox, and
 * a param write would need a new .para.dac — an image change). */
#include "common_event_data.h"
#include "common_event_manager.h"
#include "common_event_subscriber.h"
#include "matching_skills.h"
#include "want.h"

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

/* Set by OnSignal so the pre-Server startup path (QueryDisplay's wait) can
 * bail out too.  Before that wait existed the window between installing the
 * handlers and assigning g_server was ~0, so a signal arriving in it was a
 * non-issue; now it can be seconds, and a handler that does nothing would
 * swallow SIGTERM and leave the process killable only by SIGKILL. */
std::atomic<bool> g_stopRequested{false};

void OnSignal(int sig)
{
    HILOG_INFO(LOG_CORE, "signal %{public}d — stopping", sig);
    g_stopRequested.store(true);
    if (g_server != nullptr) {
        g_server->Stop();
    }
}

/*
 * Panel geometry from RS.
 *
 * We must NOT assume render_service is already up.  The old code did — its
 * comment claimed "the service is ordered after it" — but that stopped being
 * true when waydroid-supervisor.sh took over auto-start: it runs from init's
 * boot/post-fs-data stages, and render_service has been seen coming up after
 * us (ansuz: compositor pid 3415 @ 17:16:38, render_service pid 3422 @
 * 17:16:39).  Silently falling back then hands the container a wl_output of
 * the wrong size, which is what it sizes its display to.
 *
 * This wait is about GEOMETRY only.  It does not make the self-drawing node
 * reach the render tree: RS answers screen queries long before the display
 * node our AttachToDisplay needs exists, so that attach is dropped either
 * way — OutputSurface::EnsureAttachedToDisplay is what handles it.
 *
 * The budget is bounded because the wayland socket is not created until
 * after we return (Server::Init), and the supervisor abandons the generation
 * if that socket does not appear within its own 150 x 0.2 s = 30 s window.
 * 12 s leaves ample room for the rest of startup (~5-7 s observed).
 */
constexpr auto kScreenWaitBudget   = std::chrono::seconds(12);
constexpr auto kScreenWaitInterval = std::chrono::milliseconds(200);

void QueryDisplay(ServerConfig& cfg)
{
    auto& rs = Rosen::RSInterfaces::GetInstance();
    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + kScreenWaitBudget;
    bool logged = false;

    /* A real deadline, not a sleep count: every failed probe goes through
     * RSRenderServiceConnectHub::Connect(), which itself retries samgr a few
     * times with its own sleeps, so counting our sleeps would understate the
     * elapsed time and quietly eat into the supervisor's socket budget. */
    for (;;) {
        if (g_stopRequested.load()) {
            HILOG_INFO(LOG_CORE, "stop requested while waiting for a screen");
            return;
        }
        Rosen::ScreenId screenId = rs.GetDefaultScreenId();
        if (screenId != Rosen::INVALID_SCREEN_ID) {
            Rosen::RSScreenModeInfo mode = rs.GetScreenActiveMode(screenId);
            if (mode.GetScreenWidth() > 0 && mode.GetScreenHeight() > 0) {
                cfg.screenId = screenId;
                cfg.width  = mode.GetScreenWidth();
                cfg.height = mode.GetScreenHeight();
                if (mode.GetScreenRefreshRate() > 0) {
                    cfg.refreshMHz = mode.GetScreenRefreshRate() * 1000;
                }
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                HILOG_INFO(LOG_CORE,
                           "screen %{public}llu: %{public}dx%{public}d @%{public}d mHz "
                           "(after %{public}lld ms)",
                           static_cast<unsigned long long>(cfg.screenId),
                           cfg.width, cfg.height, cfg.refreshMHz,
                           static_cast<long long>(waited));
                return;
            }
        }
        if (!logged) {
            HILOG_INFO(LOG_CORE, "no screen yet — waiting for render_service");
            logged = true;
        }
        /* Check before sleeping so we never burn a final interval we cannot
         * use, and never overshoot the budget by one whole interval. */
        if (std::chrono::steady_clock::now() + kScreenWaitInterval >= deadline) {
            break;
        }
        std::this_thread::sleep_for(kScreenWaitInterval);
    }

    /*
     * Soft failure: keep the ServerConfig defaults and boot the container
     * anyway rather than failing the generation, which would only make the
     * supervisor rebuild us into the same wait.
     *
     * Those defaults (wl_server.h, 1080x2400) are the ansuz panel, so this
     * path is a latent wart on a tree whose rule is one image for both
     * devices with per-device settings coming from init.<dev>.cfg at runtime
     * — it happens to be invisible today only because the X23 never starts
     * waydroid at all.  If a second device ever enables it, plumb the
     * fallback geometry in from the per-device config instead of relying on
     * this.  Loud on purpose: silence here is what hid the original bug.
     */
    HILOG_WARN(LOG_CORE,
               "no screen after %{public}lld ms — falling back to %{public}dx%{public}d "
               "(ansuz defaults; wrong on any other panel)",
               static_cast<long long>(
                   std::chrono::duration_cast<std::chrono::milliseconds>(kScreenWaitBudget).count()),
               cfg.width, cfg.height);
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

/* Our minted native token (see GrantInputPermission).  MMI checks
 * INTERCEPT_INPUT_EVENT against the CALLING THREAD's self token, and
 * SetSelfTokenID is per-thread — so the param-watch thread that toggles the
 * grab must re-assert it before AddInterceptor/RemoveInterceptor, or the call
 * fails with EPERM (-201). */
uint64_t g_inputTokenId = 0;

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
    g_inputTokenId = tokenId;
    SetSelfTokenID(tokenId);
    Security::AccessToken::AccessTokenKit::ReloadNativeTokenInfo();
}

/* On-device escape hatch.  The touch grab is touch-ONLY, so while the
 * container owns the screen OHOS's swipe gestures (home/back) are eaten and
 * a user with no hdc has no way out of Waydroid.  Hardware keys are NOT
 * grabbed, so a key chord still reaches us: hold Volume-Down then press
 * Volume-Up.  A deliberate two-key chord — normal single volume presses
 * never trigger it, and it doesn't clash with the screenshot/power combos.
 * On fire we hide+freeze the container (touch returns to OHOS, same path as
 * the launcher's HIDE) and start the home ability so the user lands on the
 * OHOS launcher rather than the "Android is starting…" placeholder. */
int32_t g_exitChordId = -1;

/* Broadcast to the "Android Apps" launcher telling it to send itself to the
 * background.  We cannot go-home from here: StartAbility(home) does not
 * background the foreground app under sceneboard, and MinimizeAllAppWindows
 * is a silent no-op unless the launcher registered its minimize callback.
 * The one thing that reliably works is the app backgrounding ITSELF, so the
 * chord tells it to (the app subscribes and calls moveAbilityToBackground →
 * OHOS shows home, and its onBackground republishes HIDE for good measure). */
constexpr const char* kEventExit = "org.oniroproject.waydroid.EXIT";

void RegisterExitChord(Server* server)
{
    auto opt = std::make_shared<MMI::KeyOption>();
    opt->SetPreKeys({ MMI::KeyEvent::KEYCODE_VOLUME_DOWN });
    opt->SetFinalKey(MMI::KeyEvent::KEYCODE_VOLUME_UP);
    opt->SetFinalKeyDown(true);           /* fire on Vol-Up press */
    opt->SetFinalKeyDownDuration(0);      /* no long-press hold needed */
    g_exitChordId = MMI::InputManager::GetInstance()->SubscribeKeyEvent(
        opt, [server](std::shared_ptr<MMI::KeyEvent>) {
            HILOG_INFO(LOG_CORE, "exit chord (Vol-Down + Vol-Up): leaving Waydroid");
            /* 1) Immediate: release grab + hide + freeze (reuses HIDE path).
             * SubscribeKeyEvent's callback runs on an MMI thread; ApplyVisibility
             * touches params/RS but needs no special token here. */
            WaydroidSessionStub::HideAndHold(server, 3000);
            /* 2) Tell the launcher to background itself → OHOS shows home.
             * See kEventExit above for why we don't go-home from here. */
            AAFwk::Want want;
            want.SetAction(kEventExit);
            EventFwk::CommonEventData data(want);
            if (!EventFwk::CommonEventManager::PublishCommonEvent(data)) {
                HILOG_WARN(LOG_CORE, "publish EXIT failed (touch is back; swipe up)");
            }
        });
    if (g_exitChordId >= 0) {
        HILOG_INFO(LOG_CORE, "exit chord registered (Vol-Down+Vol-Up, id %{public}d)",
                   g_exitChordId);
    } else {
        HILOG_ERROR(LOG_CORE, "exit-chord SubscribeKeyEvent failed: %{public}d",
                    g_exitChordId);
    }
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
        /* Re-assert our native token on THIS thread: Apply() runs on the
         * param-watch thread, and the token set by GrantInputPermission on the
         * main thread does not carry over (it is per-thread), so MMI would
         * reject the interceptor call with -201. */
        if (g_inputTokenId != 0) {
            SetSelfTokenID(g_inputTokenId);
        }
        if (grab && interceptorId_ < 0) {
            int32_t id = MMI::InputManager::GetInstance()->AddInterceptor(
                feeder_, MMI::DEFUALT_INTERCEPTOR_PRIORITY,
                MMI::CapabilityToTags(MMI::INPUT_DEV_CAP_TOUCH));
            if (id >= 0) {
                interceptorId_ = id;
                HILOG_INFO(LOG_CORE, "touch grab on: interceptor %{public}d", id);
            } else {
                HILOG_ERROR(LOG_CORE, "AddInterceptor failed: %{public}d", id);
            }
        } else if (!grab && interceptorId_ >= 0) {
            MMI::InputManager::GetInstance()->RemoveInterceptor(interceptorId_);
            interceptorId_ = -1;
            HILOG_INFO(LOG_CORE, "touch grab off");
        }
    }

    void ApplyFromParam()
    {
        char value[8] = { 0 };
        /* Default OFF.  On auto-start (persist.waydroid.enabled=1 at boot) the
         * "Android Apps" launcher has not been foregrounded, so the param is
         * unset — touch must stay with OHOS, never be grabbed for a container
         * running in the background.  Only an explicit "1" grabs. */
        int n = GetParameter(kGrabParam, "0", value, sizeof value);
        Apply(n >= 0 && strcmp(value, "1") == 0);
    }

private:
    std::mutex mutex_;
    std::shared_ptr<TouchFeeder> feeder_;
    int32_t interceptorId_ = -1;
};

GrabController* g_grab = nullptr;

/* W5 lifecycle: the "Android Apps" launcher publishes these on
 * onForeground / onBackground; the compositor shows+thaws or hides+freezes
 * the container (and moves the touch grab to match) in response. Custom
 * (non-system) events, so neither side needs a permission or a param DAC. */
constexpr const char* kFrontEndBundle = "org.oniroproject.androidapps";
constexpr const char* kEventShow = "org.oniroproject.waydroid.SHOW";
constexpr const char* kEventHide = "org.oniroproject.waydroid.HIDE";

class VisibilityReceiver : public EventFwk::CommonEventSubscriber {
public:
    VisibilityReceiver(const EventFwk::CommonEventSubscribeInfo& info, Server* server)
        : EventFwk::CommonEventSubscriber(info), server_(server) {}

    void OnReceiveEvent(const EventFwk::CommonEventData& data) override
    {
        const std::string action = data.GetWant().GetAction();
        if (action == kEventShow) {
            WaydroidSessionStub::ApplyVisibility(server_, true);
        } else if (action == kEventHide) {
            WaydroidSessionStub::ApplyVisibility(server_, false);
        }
    }

private:
    Server* server_;
};

std::shared_ptr<VisibilityReceiver> g_visReceiver;

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

    /* Start with the container's output HIDDEN.  It still boots in the
     * background (not frozen), but its self-drawing overlay stays off-screen
     * until the "Android Apps" launcher is opened (SHOW event →
     * ApplyVisibility(true) reveals + grabs).  Without this an auto-started
     * container would overlay OHOS on boot with no way to interact with it
     * (touch is not grabbed until the app is foregrounded), and OHOS would be
     * covered.  Matches the W5 app-driven show/hide model. */
    server.Output().SetNodeVisible(false);

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

    /* On-device escape from the touch grab (Vol-Down + Vol-Up → home). */
    RegisterExitChord(&server);

    /* W5: publish the session SA so the "Android Apps" front-end can hand
     * over its XComponent surface, forward touch, and drive lifecycle. */
    WaydroidSessionStub::Publish(&server);

    /* W5 lifecycle: subscribe to the launcher's SHOW/HIDE common events so
     * the container follows the app fore/background.  Startup is the fully
     * dormant state — the node was just hidden above, the touch grab
     * defaults OFF, and the attach we issued was in all likelihood dropped
     * (see OutputSurface::EnsureAttachedToDisplay).  The first SHOW is what
     * attaches, reveals and grabs; nothing before it puts the container on
     * the panel. */
    {
        EventFwk::MatchingSkills skills;
        skills.AddEvent(kEventShow);
        skills.AddEvent(kEventHide);
        EventFwk::CommonEventSubscribeInfo info(skills);
        /* SHOW puts a fullscreen, touch-grabbing layer over OHOS, and a custom
         * common event can be published by any app: only listen to the
         * front-end.  (CES checks the publisher itself, so `cem publish` from
         * a shell no longer drives this — use the app or waydroid_session_test.
         * samgr's on-demand policy can not filter, so another app can still
         * START the stack; it just can not show it.) */
        info.SetPublisherBundleName(kFrontEndBundle);
        g_visReceiver = std::make_shared<VisibilityReceiver>(info, &server);
        if (EventFwk::CommonEventManager::SubscribeCommonEvent(g_visReceiver)) {
            HILOG_INFO(LOG_CORE, "W5: subscribed to visibility common events");
        } else {
            HILOG_WARN(LOG_CORE, "W5: subscribe visibility common events failed");
        }
    }

    /* Tell waydroidd the socket exists; it gates container start on this
     * (the hwc's own 5 s retry loop would absorb the race, but the
     * handshake keeps the logs clean — same shape as androidd's
     * android.composer.ready). */
    SetParameter("waydroid.compositor.frames", "0");
    SetParameter("waydroid.compositor.ready", "1");

    HILOG_INFO(LOG_CORE, "entering event loop");
    /* Visibility is a lease the front-end renews (waydroid_session.h). */
    server.SetTick([&server]() { WaydroidSessionStub::CheckVisibilityLease(&server); });
    server.Run();

    if (g_visReceiver != nullptr) {
        EventFwk::CommonEventManager::UnSubscribeCommonEvent(g_visReceiver);
        g_visReceiver = nullptr;
    }
    if (g_exitChordId >= 0) {
        MMI::InputManager::GetInstance()->UnsubscribeKeyEvent(g_exitChordId);
        g_exitChordId = -1;
    }
    WaydroidSessionStub::Withdraw();
    grab.Apply(false);
    g_grab = nullptr;
    SetParameter("waydroid.compositor.ready", "0");
    HILOG_INFO(LOG_CORE, "exiting");
    return 0;
}
