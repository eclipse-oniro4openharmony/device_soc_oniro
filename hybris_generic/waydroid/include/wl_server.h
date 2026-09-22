/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * The minimal Wayland server the Waydroid hwcomposer needs (plan D2/§2.6):
 * six globals plus android_wlegl. Anything the hwc treats as optional
 * (subcompositor beyond a stub, presentation, viewporter, tablet,
 * data-device) is deliberately absent — the protocol subset is the
 * maintenance budget.
 *
 *   wl_compositor   surfaces + regions
 *   wl_subcompositor stub (hwc binds it but the fullscreen path never
 *                    creates subsurfaces)
 *   wl_seat         touch + keyboard (BACK); the hwc converts our events
 *                    into evdev packets on container-internal FIFOs
 *   wl_output       one mode, from the OHOS display info
 *   wl_shm          libwayland's built-in (cursor/fallback only)
 *   xdg_wm_base     surface → toplevel, configure/ack, close
 *   android_wlegl   the buffer path (libhybris server_wlegl), chosen by
 *                   the container's ro.hardware.gralloc=android
 */

#ifndef WAYDROID_WL_SERVER_H
#define WAYDROID_WL_SERVER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <wayland-server.h>

#include "wl_output_surface.h"
#include "wlegl_import.h"

namespace OHOS {
namespace Waydroid {

struct ServerConfig {
    std::string socketPath = "/data/waydroid/run/xdg";  /* XDG_RUNTIME_DIR */
    std::string socketName = "wayland-0";
    int32_t width    = 1080;
    int32_t height   = 2400;
    int32_t refreshMHz = 60000;
    uint64_t screenId = 0;
};

/* One wl_touch protocol step, queued from the OHOS input thread and
 * replayed on the wayland thread (W4).  A PointerEvent becomes a short
 * run of these ending in Frame. */
struct TouchOp {
    enum Kind : int32_t { Down = 0, Up, Motion, Frame, Cancel };
    int32_t kind = Frame;
    int32_t id = 0;                 /* OHOS pointer id, forwarded as-is */
    int32_t x = 0;
    int32_t y = 0;
};

/*
 * One xdg_toplevel.  In the hwc's full-UI mode there is a single one
 * (app_id "Waydroid") carrying the whole Android display; in its per-task
 * modes there is one per Android task — a "card" — with app_id
 * "waydroid.<package>" and the task's label as the title.  The table is the
 * compositor's only knowledge of what Android is showing, and it is what the
 * session SA turns into taskOnTop / taskGone / allGone for the shell.
 */
struct ToplevelInfo {
    uint32_t id = 0;            /* ours; unique for the server's lifetime */
    uint32_t client = 0;        /* which wayland connection (1, 2, ...) */
    std::string appId;
    std::string title;
    bool mapped = false;        /* has committed a real (wlegl) buffer */
    uint64_t frames = 0;
};

/* Android's HOME activity (setup wizard / launcher3): has a window in the
 * hwc's per-task mode, is never shown, and its coming to the top means "the
 * app was backed out of". */
bool IsHomePackage(const std::string& package);

enum class ToplevelEvent : int32_t {
    Created = 0,
    Updated,        /* app_id or title changed */
    OnTop,          /* started presenting: its frames are what is on screen */
    Inactive,       /* stopped: the hwc parked a snapshot on its surface */
    Destroyed,
};

class Server {
public:
    Server() = default;
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /* Create the display, register the globals and bind the socket. */
    bool Init(const ServerConfig& config);

    /* Run the event loop until Stop(); dispatches wayland and flushes
     * clients. */
    void Run();
    void Stop();

    /* Called on the event-loop thread once per loop turn (at least every
     * 100 ms).  For cheap periodic checks that must not need a thread of
     * their own — the visibility lease. */
    void SetTick(std::function<void()> tick) { tick_ = std::move(tick); }

    /*
     * In-flight bookkeeping for the deferred wl_buffer.release.
     *
     * Track/Untrack run on the wayland thread; OnBufferReleased runs on
     * an RS binder thread and only queues the resource, waking the
     * wayland loop through an eventfd — wl_resource calls are not
     * thread-safe, so the actual wl_buffer.release is sent from
     * DrainReleases() on the wayland thread.
     */
    void TrackInFlight(SurfaceBuffer* buffer, struct wl_resource* wlBuffer, int32_t window = 0);
    void UntrackInFlight(SurfaceBuffer* buffer);

    /* Hand every in-flight buffer back to its client at once (wayland
     * thread only).  Used to break a QUEUE_FULL deadlock: if RS stopped
     * acquiring (screen-off) the queue never drains on its own. */
    void ReleaseEverythingInFlight();

    /* Follow a wl_buffer's lifetime so its import can be dropped when
     * the client destroys it (or dies). */
    void WatchBufferDestroy(struct wl_resource* wlBuffer);
    void ForgetBuffer(struct wl_resource* wlBuffer);

    /*
     * Touch input (W4).  InjectTouchOps is thread-safe: it queues and
     * wakes the wayland loop, which replays the ops as wl_touch events
     * to the hwc (DrainTouchOps).  The seat/surface bookkeeping calls
     * run on the wayland thread only.
     */
    void InjectTouchOps(const std::vector<TouchOp>& ops);
    void AddTouchResource(struct wl_resource* touch);
    void RemoveTouchResource(struct wl_resource* touch);
    void NoteInputSurface(struct wl_resource* surface);
    void DropInputSurface(struct wl_resource* surface);

    /* Single-pointer touch from the W5 IPC (action == OHOS PointerEvent
     * action: 1=cancel 2=down 3=move 4=up).  Emits the op run + frame. */
    void InjectTouchFromAction(int32_t action, int32_t id, int32_t x, int32_t y);

    /* W5: revert the output to the built-in self-drawing node (used when
     * the front-end app surface goes away / dies). */
    void RevertToSelfDrawing();

    /* Run fn on the wayland thread, soon.  Any thread.  libwayland is not
     * thread-safe, so everything that touches a wl_resource from an IPC,
     * param-watch or input thread goes through here. */
    void Post(std::function<void()> fn);

    /*
     * The toplevel table.  The bookkeeping calls run on the wayland thread
     * (they are the xdg_toplevel request handlers); Toplevels() is a
     * thread-safe snapshot.  The listener is called on the wayland thread
     * and must not block.
     */
    using ToplevelListener = std::function<void(ToplevelEvent, const ToplevelInfo&)>;
    void SetToplevelListener(ToplevelListener cb) { toplevelCb_ = std::move(cb); }
    std::vector<ToplevelInfo> Toplevels();

    uint32_t AddToplevel(struct wl_resource* toplevel, struct wl_resource* xdgSurface);
    void RemoveToplevel(uint32_t id);
    void SetToplevelAppId(uint32_t id, const char* appId);
    void SetToplevelTitle(uint32_t id, const char* title);
    void NoteToplevelFrame(uint32_t id);
    void NoteToplevelInactive(uint32_t id);

    /* Wayland thread (use Post).  Close asks the client to close that window
     * — the hwc answers by removing the Android task.  Configure sends a new
     * size (0 = keep) and activation state to one toplevel, or to all of
     * them with id 0. */
    void CloseToplevel(uint32_t id);
    void ConfigureToplevel(uint32_t id, int32_t width, int32_t height, bool activated);

    uint32_t NoteClientConnected();

    /*
     * Windows: one output per OHOS window (launcher plan L2–L4).
     *
     * The hwc gives every Android task its own xdg_toplevel and presents a
     * task's frames on that toplevel's surface, so routing is by window: a
     * frame goes to the window bound to its toplevel's package
     * (app_id "waydroid.<package>"), else to a window bound to "" (takes
     * whatever is on top — the single-window shell and the full desktop),
     * else to the legacy fullscreen output if one is attached (debug
     * overlay, bring-up tool), else it is dropped and the buffer handed
     * straight back.
     *
     * Attach/Detach/… may be called from any thread; what touches wayland
     * is posted to the wayland thread.
     */
    int32_t AttachWindow(const std::string& package, const sptr<IBufferProducer>& producer,
                         int32_t width, int32_t height);
    bool DetachWindow(int32_t window);
    bool WindowAlive(int32_t window);
    void WindowTouch(int32_t window, int32_t action, int32_t id, int32_t x, int32_t y);
    /* evdev key code (KEY_BACK = 158): the hwc forwards wl_keyboard codes
     * verbatim into Android's input FIFO. */
    void WindowKey(int32_t window, int32_t code, bool down);
    /* The OHOS window came to the front / went away: (de)activate the
     * toplevel it shows, which makes the hwc switch Android's focused task. */
    void SetWindowActive(int32_t window, bool active);
    /* Ask the hwc to close every window of `package`; it removes the task. */
    void CloseApp(const std::string& package);

    /* wl_output bookkeeping (wayland thread). */
    void AddOutputResource(struct wl_resource* output);
    void RemoveOutputResource(struct wl_resource* output);

    /* wl_keyboard bookkeeping (wayland thread). */
    void AddKeyboardResource(struct wl_resource* keyboard);
    void RemoveKeyboardResource(struct wl_resource* keyboard);

    /* Frame path (wayland thread): where does a frame of this toplevel go?
     * nullptr = nowhere.  The shared_ptr keeps a detaching output alive for
     * the flush in progress. */
    std::shared_ptr<OutputSurface> OutputFor(uint32_t toplevelId, int32_t* windowOut);
    void NoteToplevelSurface(uint32_t toplevelId, struct wl_resource* surface);
    void ReleaseInFlightOf(int32_t window);
    /* What a toplevel is configured to (see outputWidth_). */
    void OutputSize(int32_t* width, int32_t* height);

    OutputSurface& Output() { return *output_; }
    BufferImporter& Importer() { return importer_; }
    const ServerConfig& Config() const { return config_; }

    struct wl_display* Display() { return display_; }

private:
    bool CreateGlobals();
    void OnBufferReleased(SurfaceBuffer* buffer);
    void DrainReleases();
    void DrainTouchOps();

    ServerConfig config_;
    struct wl_display* display_ = nullptr;
    /* The legacy fullscreen output: the debug overlay's self-drawing node or
     * the bring-up tool's producer.  Unattached in the product path. */
    std::shared_ptr<OutputSurface> output_ = std::make_shared<OutputSurface>();
    BufferImporter importer_;

    struct Window {
        int32_t id = 0;
        std::string package;            /* "" = whatever is on top */
        std::shared_ptr<OutputSurface> output;
        sptr<IRemoteObject> producerObject;
        sptr<IRemoteObject::DeathRecipient> death;
        uint32_t toplevelId = 0;        /* whose frames it showed last */
        bool active = true;
    };
    void SendKeyboardFocus(struct wl_resource* surface);

    void ApplyActivation(bool edge);
    void SendSurfaceOnOutput(uint32_t toplevelId, bool shown);

    std::mutex windowsMutex_;
    std::unordered_map<int32_t, std::shared_ptr<Window>> windows_;
    int32_t nextWindowId_ = 1;
    int32_t activeWindow_ = 0;          /* the OHOS window in front, if ours */
    std::string activePackage_;         /* its package; "*" = whatever is on top */
    /* What every toplevel is configured to: the attached window's size once
     * there is one, the panel until then.  The hwc resizes Android's display
     * to the LAST configure it saw from any toplevel, so they must agree. */
    int32_t outputWidth_ = 0;
    int32_t outputHeight_ = 0;

    struct InFlight {
        struct wl_resource* wlBuffer = nullptr;
        int32_t window = 0;
    };
    std::mutex inFlightMutex_;
    std::unordered_map<SurfaceBuffer*, InFlight> inFlight_;
    std::vector<struct wl_resource*> releasedPending_;
    std::unordered_set<struct wl_resource*> watchedBuffers_;
    int releaseEventFd_ = -1;

    /* Touch: pending ops (input thread → wayland thread), the hwc's
     * wl_touch resources, and the surface events are addressed to (the
     * last one that committed a wlegl buffer — the fullscreen SF
     * framebuffer-target in full-UI mode). */
    std::mutex touchMutex_;
    std::vector<TouchOp> touchPending_;
    int touchEventFd_ = -1;
    std::vector<struct wl_resource*> touchResources_;
    struct wl_resource* inputSurface_ = nullptr;
    std::vector<struct wl_resource*> outputResources_;
    std::vector<struct wl_resource*> keyboardResources_;
    struct wl_resource* keyboardFocus_ = nullptr;

    struct ToplevelRec {
        ToplevelInfo info;
        struct wl_resource* toplevel = nullptr;
        struct wl_resource* xdgSurface = nullptr;
        struct wl_resource* surface = nullptr;   /* the wl_surface it presents on */
        bool activated = false;                  /* what we last configured */
        bool entered = false;                    /* wl_surface.enter sent */
        bool home = false;                       /* Android's HOME: never shown */
    };
    void EmitToplevel(ToplevelEvent ev, const ToplevelInfo& info);
    void DrainPosted();

    std::mutex toplevelMutex_;      /* guards toplevels_ for Toplevels() */
    std::unordered_map<uint32_t, ToplevelRec> toplevels_;
    uint32_t nextToplevelId_ = 1;
    uint32_t onTopId_ = 0;          /* the toplevel whose frames we show */
    uint32_t clients_ = 0;
    ToplevelListener toplevelCb_;

    std::mutex postedMutex_;
    std::vector<std::function<void()>> posted_;
    int postedEventFd_ = -1;

    bool running_ = false;
    std::function<void()> tick_;
};

} // namespace Waydroid
} // namespace OHOS

#endif // WAYDROID_WL_SERVER_H
