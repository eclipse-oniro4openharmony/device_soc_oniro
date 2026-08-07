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

    /* Follow a wl_buffer's lifetime so its import can be dropped when
     * the client destroys it (or dies). */
    void WatchBufferDestroy(struct wl_resource* wlBuffer);
    void ForgetBuffer(struct wl_resource* wlBuffer);

    OutputSurface& Output() { return output_; }
    BufferImporter& Importer() { return importer_; }
    const ServerConfig& Config() const { return config_; }

    struct wl_display* Display() { return display_; }

private:
    bool CreateGlobals();
    void OnBufferReleased(SurfaceBuffer* buffer);
    void DrainReleases();

    ServerConfig config_;
    struct wl_display* display_ = nullptr;
    OutputSurface  output_;
    BufferImporter importer_;

    std::mutex inFlightMutex_;
    std::unordered_map<SurfaceBuffer*, struct wl_resource*> inFlight_;
    std::vector<struct wl_resource*> releasedPending_;
    std::unordered_set<struct wl_resource*> watchedBuffers_;
    int releaseEventFd_ = -1;

    bool running_ = false;
};

} // namespace Waydroid
} // namespace OHOS

#endif // WAYDROID_WL_SERVER_H
