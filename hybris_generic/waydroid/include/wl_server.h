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
 *   wl_seat         touch + keyboard; the hwc converts our events into
 *                    evdev packets on container-internal FIFOs
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
    void TrackInFlight(SurfaceBuffer* buffer, struct wl_resource* wlBuffer);
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

    OutputSurface& Output() { return output_; }
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
    OutputSurface  output_;
    BufferImporter importer_;

    std::mutex inFlightMutex_;
    std::unordered_map<SurfaceBuffer*, struct wl_resource*> inFlight_;
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

    bool running_ = false;
    std::function<void()> tick_;
};

} // namespace Waydroid
} // namespace OHOS

#endif // WAYDROID_WL_SERVER_H
