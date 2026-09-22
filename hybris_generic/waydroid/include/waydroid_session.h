/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The compositor's IPC face to the "Android Apps" shell (SA 9601).
 *
 * The compositor is an init-started native root service (sandbox:0) — it
 * must not be an app process, because its per-frame gralloc import needs
 * the /android/vendor mount that appspawn sandboxes lack.  So the shell and
 * the compositor are necessarily separate processes:
 *
 *   shell (hap) ── requireNapi('oniro.androidcontainer') ──┐   (napi/, a
 *     XComponent SURFACE → surfaceId → producer            │    system module:
 *     onTouch / onBackPress                                │    loaded outside
 *     app grid                                             │    the app's linker
 *                                                          ▼    namespace)
 *   compositor (this SA)
 *     AttachWindow(package, producer, w, h)  → frames of that Android task
 *     WindowTouch / WindowKey                → wl_touch / wl_keyboard
 *     ListApps / GetAppIcon / LaunchApp / …  → libwdbinder → IPlatform
 *
 * v1 (codes 1–4) drove one fullscreen output and is kept for the bring-up
 * tool; v2 (codes 10+) is the launcher slice: one output PER OHOS WINDOW,
 * each bound to an Android package, plus the control plane.  Plan:
 * docs/hybris_generic/android_app_launcher_plan.md.
 *
 * Who may call.  selinux is disabled on this image and a system NAPI module
 * can be loaded by any app, so the stub is its own gate: every request must
 * come from the shell bundle's HAP token or from a root native process
 * (OnRemoteRequest).  Everything the container can do for a caller —
 * installApp, launchIntent — is behind that check.
 *
 * Events go the other way as a common event restricted to the shell bundle
 * (org.oniroproject.waydroid.TASK, see PublishTaskEvent): they are rare, and
 * it spares the app hosting a binder stub.
 */

#ifndef WAYDROID_SESSION_H
#define WAYDROID_SESSION_H

#include <string>
#include <vector>

#include <iremote_broker.h>
#include <iremote_proxy.h>
#include <iremote_stub.h>

namespace OHOS {
namespace Waydroid {

class Server;
struct ToplevelInfo;
enum class ToplevelEvent : int32_t;

/* Free SAID (1..0x00ffffff), not present in system_ability_definition.h
 * nor the product's sa_install_info.json. */
constexpr int32_t WAYDROID_SESSION_SA_ID = 9601;

/* Results of the v2 calls (>= 0 is success / a value). */
enum SessionError : int32_t {
    SESSION_OK = 0,
    SESSION_EGENERIC = -1,
    SESSION_EDENIED = -2,       /* caller is not the shell */
    SESSION_EFROZEN = -3,       /* container frozen: no front-end in the foreground */
    SESSION_ENOTREADY = -4,     /* Android not booted (yet) */
    SESSION_ETIMEDOUT = -5,
    SESSION_ENOWINDOW = -6,
    SESSION_EINVAL = -7,
};

enum SessionState : int32_t {
    SESSION_STATE_BOOTING = 0,  /* container up, Android's IPlatform not registered yet */
    SESSION_STATE_READY = 1,
    SESSION_STATE_FROZEN = 2,   /* cannot tell: nothing of ours is in the foreground */
};

struct SessionApp {
    std::string name;
    std::string package;
};

class IWaydroidSession : public IRemoteBroker {
public:
    DECLARE_INTERFACE_DESCRIPTOR(u"ohos.waydroid.IWaydroidSession");

    enum Code : uint32_t {
        /* v1 — one fullscreen output (bring-up tool) */
        SET_OUTPUT_SURFACE = 1,   /* in: remote object (IBufferProducer) */
        CLEAR_OUTPUT_SURFACE,
        INJECT_TOUCH,             /* in: action, id, x, y */
        SET_FOREGROUND,           /* in: bool (unfreeze/freeze container) */

        /* v2 — the launcher slice */
        GET_STATE = 10,
        LIST_APPS,
        GET_APP_ICON,             /* in: package; out: result, PNG bytes */
        LAUNCH_APP,               /* in: package */
        CLOSE_APP,                /* in: package — ends the Android task */
        ATTACH_WINDOW,            /* in: package ("" = whatever is on top), producer, w, h */
        DETACH_WINDOW,            /* in: window */
        WINDOW_TOUCH,             /* in: window, action, id, x, y  (oneway) */
        WINDOW_KEY,               /* in: window, evdev code, down  (oneway) */
        SET_WINDOW_ACTIVE,        /* in: window, bool */
        WINDOW_ALIVE,             /* in: window */
    };

    /* Touch action matches OHOS PointerEvent action codes so the app can
     * forward them verbatim. */
    virtual int32_t SetOutputSurface(const sptr<IRemoteObject>& producer) = 0;
    virtual int32_t ClearOutputSurface() = 0;
    virtual int32_t InjectTouch(int32_t action, int32_t id, int32_t x, int32_t y) = 0;
    virtual int32_t SetForeground(bool foreground) = 0;

    virtual int32_t GetState() = 0;
    virtual int32_t ListApps(std::vector<SessionApp>& apps) = 0;
    virtual int32_t GetAppIcon(const std::string& package, std::vector<uint8_t>& png) = 0;
    virtual int32_t LaunchApp(const std::string& package) = 0;
    virtual int32_t CloseApp(const std::string& package) = 0;
    /* Returns the window id (> 0). */
    virtual int32_t AttachWindow(const std::string& package, const sptr<IRemoteObject>& producer,
                                 int32_t width, int32_t height) = 0;
    virtual int32_t DetachWindow(int32_t window) = 0;
    virtual int32_t WindowTouch(int32_t window, int32_t action, int32_t id, int32_t x,
                                int32_t y) = 0;
    virtual int32_t WindowKey(int32_t window, int32_t code, bool down) = 0;
    virtual int32_t SetWindowActive(int32_t window, bool active) = 0;
    virtual int32_t WindowAlive(int32_t window) = 0;
};

/* Hosted by the compositor. */
class WaydroidSessionStub : public IRemoteStub<IWaydroidSession> {
public:
    explicit WaydroidSessionStub(Server* server) : server_(server) {}

    int OnRemoteRequest(uint32_t code, MessageParcel& data, MessageParcel& reply,
                        MessageOption& option) override;

    int32_t SetOutputSurface(const sptr<IRemoteObject>& producer) override;
    int32_t ClearOutputSurface() override;
    int32_t InjectTouch(int32_t action, int32_t id, int32_t x, int32_t y) override;
    int32_t SetForeground(bool foreground) override;

    int32_t GetState() override;
    int32_t ListApps(std::vector<SessionApp>& apps) override;
    int32_t GetAppIcon(const std::string& package, std::vector<uint8_t>& png) override;
    int32_t LaunchApp(const std::string& package) override;
    int32_t CloseApp(const std::string& package) override;
    int32_t AttachWindow(const std::string& package, const sptr<IRemoteObject>& producer,
                         int32_t width, int32_t height) override;
    int32_t DetachWindow(int32_t window) override;
    int32_t WindowTouch(int32_t window, int32_t action, int32_t id, int32_t x, int32_t y) override;
    int32_t WindowKey(int32_t window, int32_t code, bool down) override;
    int32_t SetWindowActive(int32_t window, bool active) override;
    int32_t WindowAlive(int32_t window) override;

    /* Publish/withdraw the SA with samgr. */
    static bool Publish(Server* server);
    static void Withdraw();

    /*
     * Visibility = "a front-end ability is in the foreground".  The shell
     * republishes SHOW every couple of seconds while that is true and HIDE
     * when it stops being true; the compositor's subscriber calls this.
     *
     *   visible : thaw the container
     *   hidden  : freeze it — a hidden Android session costs ~0 CPU
     *
     * It is published as waydroid.session.visible for the supervisor's
     * idle-stop.  With the debug overlay on (waydroid.debug.overlay=1) it
     * additionally reveals/hides the fullscreen self-drawing node and moves
     * the global touch grab, which is what it used to do for everyone.
     */
    static void ApplyVisibility(Server* server, bool visible);

    /* Visibility is a LEASE, not a latch: a SHOW when already visible only
     * renews it.  If it runs out — the front-end crashed or was killed and
     * so never said HIDE — freeze (and, overlay on, give the panel and touch
     * back) instead of leaving the container running until reboot.  Call
     * from the event loop (Server::SetTick). */
    static void CheckVisibilityLease(Server* server);

    /* Hide now and ignore SHOW for holdMs (the overlay's exit chord). */
    static void HideAndHold(Server* server, int64_t holdMs);

    /* Toplevel table → org.oniroproject.waydroid.TASK for the shell.  Called
     * on the wayland thread; publishes from a worker. */
    static void OnToplevelEvent(Server* server, ToplevelEvent ev, const ToplevelInfo& info);

    /* Once per generation, as soon as Android answers: mark it provisioned
     * (no setup wizard) and hand screen power to OHOS.  Runs on its own
     * thread until done; Stop() ends it. */
    static void StartControlPlane(Server* server);
    static void StopControlPlane();

private:
    Server* server_;
};

/* Client side — used by the NAPI module and the bring-up tool. */
class WaydroidSessionProxy : public IRemoteProxy<IWaydroidSession> {
public:
    explicit WaydroidSessionProxy(const sptr<IRemoteObject>& impl)
        : IRemoteProxy<IWaydroidSession>(impl) {}

    int32_t SetOutputSurface(const sptr<IRemoteObject>& producer) override;
    int32_t ClearOutputSurface() override;
    int32_t InjectTouch(int32_t action, int32_t id, int32_t x, int32_t y) override;
    int32_t SetForeground(bool foreground) override;

    int32_t GetState() override;
    int32_t ListApps(std::vector<SessionApp>& apps) override;
    int32_t GetAppIcon(const std::string& package, std::vector<uint8_t>& png) override;
    int32_t LaunchApp(const std::string& package) override;
    int32_t CloseApp(const std::string& package) override;
    int32_t AttachWindow(const std::string& package, const sptr<IRemoteObject>& producer,
                         int32_t width, int32_t height) override;
    int32_t DetachWindow(int32_t window) override;
    int32_t WindowTouch(int32_t window, int32_t action, int32_t id, int32_t x, int32_t y) override;
    int32_t WindowKey(int32_t window, int32_t code, bool down) override;
    int32_t SetWindowActive(int32_t window, bool active) override;
    int32_t WindowAlive(int32_t window) override;

    /* CheckSystemAbility(WAYDROID_SESSION_SA_ID) + cast; nullptr if the
     * compositor is not up.  Check, not Get: Get would make samgr start the
     * on-demand stack and block for it. */
    static sptr<IWaydroidSession> Get();

private:
    static inline BrokerDelegator<WaydroidSessionProxy> delegator_;
};

} // namespace Waydroid
} // namespace OHOS

#endif // WAYDROID_SESSION_H
