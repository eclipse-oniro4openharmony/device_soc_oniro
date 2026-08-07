/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Globals + the per-commit frame path.  See wl_server.h for which parts
 * of the protocol we implement and why.
 */

#include "wl_server.h"

#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hilog/log.h>

#include "wayland-server-protocol.h"
#include "xdg-shell-server-protocol.h"

/* libhybris' server-side android_wlegl */
#include "server_wlegl.h"
#include "server_wlegl_buffer.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "waydroid_server"

namespace OHOS {
namespace Waydroid {

namespace {

constexpr uint32_t COMPOSITOR_VERSION    = 4;
constexpr uint32_t SUBCOMPOSITOR_VERSION = 1;
constexpr uint32_t SEAT_VERSION          = 5;
constexpr uint32_t OUTPUT_VERSION        = 3;
constexpr uint32_t XDG_WM_BASE_VERSION   = 3;

Server* g_server = nullptr;

/* mkdir -p: the socket dir sits under /data/waydroid, which does not
 * exist yet on a device that has never run the container. */
bool MakeDirPath(const std::string& path, mode_t mode)
{
    std::string acc;
    size_t pos = 0;
    while (pos != std::string::npos) {
        pos = path.find('/', pos + 1);
        acc = path.substr(0, pos);
        if (acc.empty()) {
            continue;
        }
        if (mkdir(acc.c_str(), mode) < 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}

/* ---- wl_surface -------------------------------------------------------- */

/* Pending (double-buffered) state, applied on commit — the protocol
 * requires attach/damage/frame to take effect only at commit. */
struct SurfaceState {
    struct wl_resource* buffer = nullptr;
    struct wl_resource* frameCallback = nullptr;
    bool bufferChanged = false;
};

struct Surface {
    struct wl_resource* resource = nullptr;
    SurfaceState pending;
    struct wl_resource* current = nullptr;   /* committed wl_buffer */
};

void SurfaceDestroy(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void SurfaceAttach(struct wl_client*, struct wl_resource* resource,
                   struct wl_resource* buffer, int32_t, int32_t)
{
    auto* surf = static_cast<Surface*>(wl_resource_get_user_data(resource));
    surf->pending.buffer = buffer;
    surf->pending.bufferChanged = true;
}

void SurfaceDamage(struct wl_client*, struct wl_resource*,
                   int32_t, int32_t, int32_t, int32_t) {}

void SurfaceFrame(struct wl_client* client, struct wl_resource* resource,
                  uint32_t callback)
{
    auto* surf = static_cast<Surface*>(wl_resource_get_user_data(resource));
    surf->pending.frameCallback =
        wl_resource_create(client, &wl_callback_interface, 1, callback);
}

void SurfaceSetOpaque(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void SurfaceSetInput(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void SurfaceSetBufferTransform(struct wl_client*, struct wl_resource*, int32_t) {}
void SurfaceSetBufferScale(struct wl_client*, struct wl_resource*, int32_t) {}
void SurfaceDamageBuffer(struct wl_client*, struct wl_resource*,
                         int32_t, int32_t, int32_t, int32_t) {}
void SurfaceOffset(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}

void SurfaceCommit(struct wl_client*, struct wl_resource* resource)
{
    auto* surf = static_cast<Surface*>(wl_resource_get_user_data(resource));
    Server* server = g_server;

    if (surf->pending.bufferChanged) {
        surf->current = surf->pending.buffer;
        surf->pending.bufferChanged = false;
    }

    if (server != nullptr && surf->current != nullptr) {
        server_wlegl_buffer* wlegl = server_wlegl_buffer_from(surf->current);
        if (wlegl != nullptr && wlegl->buf != nullptr) {
            auto* rwb = wlegl->buf;
            WleglBufferDesc desc;
            desc.width  = rwb->width;
            desc.height = rwb->height;
            desc.stride = rwb->stride;
            desc.format = rwb->format;
            desc.usage  = rwb->usage;

            server->WatchBufferDestroy(surf->current);
            sptr<SurfaceBuffer> sb = server->Importer().Import(
                surf->current, rwb->handle, desc);
            if (sb != nullptr) {
                /* The client owns the buffer until we send release, and
                 * RS is not done with it until its release listener
                 * fires — so record the mapping and let OnBufferReleased
                 * send wl_buffer.release. */
                server->TrackInFlight(sb.GetRefPtr(), surf->current);
                if (!server->Output().Flush(sb, -1, desc.width, desc.height)) {
                    /* Nothing will ever release it: hand it straight
                     * back or the client stalls forever. */
                    server->UntrackInFlight(sb.GetRefPtr());
                    wl_buffer_send_release(surf->current);
                }
            }
        }
        surf->current = nullptr;
    }

    if (surf->pending.frameCallback != nullptr) {
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t ms = static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        wl_callback_send_done(surf->pending.frameCallback, ms);
        wl_resource_destroy(surf->pending.frameCallback);
        surf->pending.frameCallback = nullptr;
    }
}

const struct wl_surface_interface g_surfaceImpl = {
    SurfaceDestroy,
    SurfaceAttach,
    SurfaceDamage,
    SurfaceFrame,
    SurfaceSetOpaque,
    SurfaceSetInput,
    SurfaceCommit,
    SurfaceSetBufferTransform,
    SurfaceSetBufferScale,
    SurfaceDamageBuffer,
    SurfaceOffset,
};

void SurfaceResourceDestroy(struct wl_resource* resource)
{
    auto* surf = static_cast<Surface*>(wl_resource_get_user_data(resource));
    delete surf;
}

/* ---- wl_region (accepted and ignored) ---------------------------------- */

void RegionDestroy(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
void RegionAdd(struct wl_client*, struct wl_resource*,
               int32_t, int32_t, int32_t, int32_t) {}
void RegionSubtract(struct wl_client*, struct wl_resource*,
                    int32_t, int32_t, int32_t, int32_t) {}

const struct wl_region_interface g_regionImpl = {
    RegionDestroy, RegionAdd, RegionSubtract,
};

/* ---- wl_compositor ------------------------------------------------------ */

void CompositorCreateSurface(struct wl_client* client, struct wl_resource* resource,
                             uint32_t id)
{
    auto* surf = new Surface();
    struct wl_resource* res = wl_resource_create(
        client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (res == nullptr) {
        delete surf;
        wl_client_post_no_memory(client);
        return;
    }
    surf->resource = res;
    wl_resource_set_implementation(res, &g_surfaceImpl, surf, SurfaceResourceDestroy);
}

void CompositorCreateRegion(struct wl_client* client, struct wl_resource* resource,
                            uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &wl_region_interface, wl_resource_get_version(resource), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &g_regionImpl, nullptr, nullptr);
}

const struct wl_compositor_interface g_compositorImpl = {
    CompositorCreateSurface, CompositorCreateRegion,
};

void CompositorBind(struct wl_client* client, void*, uint32_t version, uint32_t id)
{
    struct wl_resource* res =
        wl_resource_create(client, &wl_compositor_interface,
                           static_cast<int>(version), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &g_compositorImpl, nullptr, nullptr);
}

/* ---- wl_subcompositor (stub) -------------------------------------------- */

void SubcompositorDestroy(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void SubsurfaceDestroy(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
void SubsurfaceSetPosition(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}
void SubsurfacePlaceAbove(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void SubsurfacePlaceBelow(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void SubsurfaceSetSync(struct wl_client*, struct wl_resource*) {}
void SubsurfaceSetDesync(struct wl_client*, struct wl_resource*) {}

const struct wl_subsurface_interface g_subsurfaceImpl = {
    SubsurfaceDestroy, SubsurfaceSetPosition, SubsurfacePlaceAbove,
    SubsurfacePlaceBelow, SubsurfaceSetSync, SubsurfaceSetDesync,
};

void SubcompositorGetSubsurface(struct wl_client* client, struct wl_resource* resource,
                                uint32_t id, struct wl_resource*, struct wl_resource*)
{
    struct wl_resource* res = wl_resource_create(
        client, &wl_subsurface_interface, wl_resource_get_version(resource), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &g_subsurfaceImpl, nullptr, nullptr);
}

const struct wl_subcompositor_interface g_subcompositorImpl = {
    SubcompositorDestroy, SubcompositorGetSubsurface,
};

void SubcompositorBind(struct wl_client* client, void*, uint32_t version, uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &wl_subcompositor_interface, static_cast<int>(version), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &g_subcompositorImpl, nullptr, nullptr);
}

/* ---- wl_output ---------------------------------------------------------- */

void OutputRelease(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

const struct wl_output_interface g_outputImpl = { OutputRelease };

void OutputBind(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto* cfg = static_cast<ServerConfig*>(data);
    struct wl_resource* res = wl_resource_create(
        client, &wl_output_interface, static_cast<int>(version), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &g_outputImpl, nullptr, nullptr);

    /* Physical size is a lie we can afford: the container takes its DPI
     * from ro.sf.lcd_density, which waydroidd sets. */
    wl_output_send_geometry(res, 0, 0, 68, 151, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "Oniro", "Waydroid", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        cfg->width, cfg->height, cfg->refreshMHz);
    if (version >= 2) {
        wl_output_send_scale(res, 1);
        wl_output_send_done(res);
    }
}

/* ---- wl_seat ------------------------------------------------------------ */

void SeatGetPointer(struct wl_client* client, struct wl_resource* resource, uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    if (res != nullptr) {
        wl_resource_set_implementation(res, nullptr, nullptr, nullptr);
    }
}

void SeatGetKeyboard(struct wl_client* client, struct wl_resource* resource, uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (res != nullptr) {
        wl_resource_set_implementation(res, nullptr, nullptr, nullptr);
    }
}

void SeatGetTouch(struct wl_client* client, struct wl_resource* resource, uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &wl_touch_interface, wl_resource_get_version(resource), id);
    if (res != nullptr) {
        wl_resource_set_implementation(res, nullptr, nullptr, nullptr);
    }
}

void SeatRelease(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

const struct wl_seat_interface g_seatImpl = {
    SeatGetPointer, SeatGetKeyboard, SeatGetTouch, SeatRelease,
};

void SeatBind(struct wl_client* client, void*, uint32_t version, uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &wl_seat_interface, static_cast<int>(version), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &g_seatImpl, nullptr, nullptr);
    wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_TOUCH |
                                   WL_SEAT_CAPABILITY_KEYBOARD);
    if (version >= 2) {
        wl_seat_send_name(res, "waydroid-seat");
    }
}

/* ---- xdg_wm_base -------------------------------------------------------- */

void XdgToplevelDestroy(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
void XdgToplevelSetParent(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void XdgToplevelSetTitle(struct wl_client*, struct wl_resource*, const char*) {}
void XdgToplevelSetAppId(struct wl_client*, struct wl_resource*, const char*) {}
void XdgToplevelShowWindowMenu(struct wl_client*, struct wl_resource*,
                               struct wl_resource*, uint32_t, int32_t, int32_t) {}
void XdgToplevelMove(struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t) {}
void XdgToplevelResize(struct wl_client*, struct wl_resource*, struct wl_resource*,
                       uint32_t, uint32_t) {}
void XdgToplevelSetMaxSize(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}
void XdgToplevelSetMinSize(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}
void XdgToplevelSetMaximized(struct wl_client*, struct wl_resource*) {}
void XdgToplevelUnsetMaximized(struct wl_client*, struct wl_resource*) {}
void XdgToplevelSetFullscreen(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void XdgToplevelUnsetFullscreen(struct wl_client*, struct wl_resource*) {}
void XdgToplevelSetMinimized(struct wl_client*, struct wl_resource*) {}

const struct xdg_toplevel_interface g_xdgToplevelImpl = {
    XdgToplevelDestroy, XdgToplevelSetParent, XdgToplevelSetTitle,
    XdgToplevelSetAppId, XdgToplevelShowWindowMenu, XdgToplevelMove,
    XdgToplevelResize, XdgToplevelSetMaxSize, XdgToplevelSetMinSize,
    XdgToplevelSetMaximized, XdgToplevelUnsetMaximized,
    XdgToplevelSetFullscreen, XdgToplevelUnsetFullscreen,
    XdgToplevelSetMinimized,
};

void XdgSurfaceDestroy(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void XdgSurfaceGetToplevel(struct wl_client* client, struct wl_resource* resource,
                           uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &g_xdgToplevelImpl, nullptr, nullptr);

    /* Tell the container it is fullscreen at panel size right away — the
     * hwc waits for this configure before it commits anything. */
    struct wl_array states;
    wl_array_init(&states);
    if (uint32_t* s = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)))) {
        *s = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    if (uint32_t* s = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)))) {
        *s = XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    const ServerConfig& cfg = g_server->Config();
    xdg_toplevel_send_configure(res, cfg.width, cfg.height, &states);
    wl_array_release(&states);

    xdg_surface_send_configure(resource, wl_display_next_serial(g_server->Display()));
}

void XdgSurfaceGetPopup(struct wl_client*, struct wl_resource*, uint32_t,
                        struct wl_resource*, struct wl_resource*) {}
void XdgSurfaceSetWindowGeometry(struct wl_client*, struct wl_resource*,
                                 int32_t, int32_t, int32_t, int32_t) {}
void XdgSurfaceAckConfigure(struct wl_client*, struct wl_resource*, uint32_t) {}

const struct xdg_surface_interface g_xdgSurfaceImpl = {
    XdgSurfaceDestroy, XdgSurfaceGetToplevel, XdgSurfaceGetPopup,
    XdgSurfaceSetWindowGeometry, XdgSurfaceAckConfigure,
};

void XdgWmBaseDestroy(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void XdgWmBaseCreatePositioner(struct wl_client* client, struct wl_resource* resource,
                               uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    if (res != nullptr) {
        wl_resource_set_implementation(res, nullptr, nullptr, nullptr);
    }
}

void XdgWmBaseGetXdgSurface(struct wl_client* client, struct wl_resource* resource,
                            uint32_t id, struct wl_resource*)
{
    struct wl_resource* res = wl_resource_create(
        client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &g_xdgSurfaceImpl, nullptr, nullptr);
}

void XdgWmBasePong(struct wl_client*, struct wl_resource*, uint32_t) {}

const struct xdg_wm_base_interface g_xdgWmBaseImpl = {
    XdgWmBaseDestroy, XdgWmBaseCreatePositioner, XdgWmBaseGetXdgSurface,
    XdgWmBasePong,
};

void XdgWmBaseBind(struct wl_client* client, void*, uint32_t version, uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &xdg_wm_base_interface, static_cast<int>(version), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &g_xdgWmBaseImpl, nullptr, nullptr);
}

} // namespace

/* ------------------------------------------------------------------------ */

Server::~Server()
{
    if (releaseEventFd_ >= 0) {
        close(releaseEventFd_);
        releaseEventFd_ = -1;
    }
    if (display_ != nullptr) {
        wl_display_destroy(display_);
        display_ = nullptr;
    }
    if (g_server == this) {
        g_server = nullptr;
    }
}


/* A wl_buffer lives only as long as its client.  When one goes away its
 * imported SurfaceBuffer must go too, or the importer cache — and the
 * producer queue slots those buffers occupy — grow until every flush
 * fails with BUFFER_QUEUE_FULL. */
void Server::WatchBufferDestroy(struct wl_resource* wlBuffer)
{
    if (watchedBuffers_.count(wlBuffer) != 0) {
        return;
    }
    auto* listener = new struct wl_listener();
    listener->notify = [](struct wl_listener* l, void* data) {
        auto* res = static_cast<struct wl_resource*>(data);
        if (g_server != nullptr) {
            g_server->ForgetBuffer(res);
        }
        wl_list_remove(&l->link);
        delete l;
    };
    wl_resource_add_destroy_listener(wlBuffer, listener);
    watchedBuffers_.insert(wlBuffer);
}

void Server::ForgetBuffer(struct wl_resource* wlBuffer)
{
    watchedBuffers_.erase(wlBuffer);
    importer_.Forget(wlBuffer);
    {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        for (auto it = inFlight_.begin(); it != inFlight_.end();) {
            it = (it->second == wlBuffer) ? inFlight_.erase(it) : std::next(it);
        }
        for (auto it = releasedPending_.begin(); it != releasedPending_.end();) {
            it = (*it == wlBuffer) ? releasedPending_.erase(it) : std::next(it);
        }
    }
    /* The queue still holds slots for this client's buffers and nothing
     * will release them now — start over. */
    output_.ResetQueue();
}

/* ---- deferred wl_buffer.release ---------------------------------------- */

void Server::TrackInFlight(SurfaceBuffer* buffer, struct wl_resource* wlBuffer)
{
    std::lock_guard<std::mutex> lock(inFlightMutex_);
    inFlight_[buffer] = wlBuffer;
}

void Server::UntrackInFlight(SurfaceBuffer* buffer)
{
    std::lock_guard<std::mutex> lock(inFlightMutex_);
    inFlight_.erase(buffer);
}

/* RS binder thread: queue the resource and wake the wayland loop.  No
 * wl_* call may happen here — libwayland is not thread-safe. */
void Server::OnBufferReleased(SurfaceBuffer* buffer)
{
    {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        auto it = inFlight_.find(buffer);
        if (it == inFlight_.end()) {
            return;
        }
        releasedPending_.push_back(it->second);
        inFlight_.erase(it);
    }
    if (releaseEventFd_ >= 0) {
        uint64_t one = 1;
        (void)!write(releaseEventFd_, &one, sizeof one);
    }
}

/* Wayland thread. */
void Server::DrainReleases()
{
    std::vector<struct wl_resource*> pending;
    {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        pending.swap(releasedPending_);
    }
    for (struct wl_resource* res : pending) {
        wl_buffer_send_release(res);
    }
    if (!pending.empty()) {
        wl_display_flush_clients(display_);
    }
}

bool Server::CreateGlobals()
{
    if (wl_global_create(display_, &wl_compositor_interface, COMPOSITOR_VERSION,
                         nullptr, CompositorBind) == nullptr ||
        wl_global_create(display_, &wl_subcompositor_interface, SUBCOMPOSITOR_VERSION,
                         nullptr, SubcompositorBind) == nullptr ||
        wl_global_create(display_, &wl_seat_interface, SEAT_VERSION,
                         nullptr, SeatBind) == nullptr ||
        wl_global_create(display_, &wl_output_interface, OUTPUT_VERSION,
                         &config_, OutputBind) == nullptr ||
        wl_global_create(display_, &xdg_wm_base_interface, XDG_WM_BASE_VERSION,
                         nullptr, XdgWmBaseBind) == nullptr) {
        HILOG_ERROR(LOG_CORE, "CreateGlobals: wl_global_create failed");
        return false;
    }

    /* libwayland's built-in wl_shm (cursor / fallback path only). */
    if (wl_display_init_shm(display_) < 0) {
        HILOG_ERROR(LOG_CORE, "CreateGlobals: wl_display_init_shm failed");
        return false;
    }

    /* The buffer path: libhybris' server-side android_wlegl.  Chosen by
     * the container because waydroidd sets ro.hardware.gralloc=android. */
    if (server_wlegl_create(display_) == nullptr) {
        HILOG_ERROR(LOG_CORE, "CreateGlobals: server_wlegl_create failed");
        return false;
    }
    return true;
}

bool Server::Init(const ServerConfig& config)
{
    config_ = config;
    g_server = this;

    /* The socket dir is what waydroidd bind-mounts into the container at
     * /run/xdg; libwayland takes it from XDG_RUNTIME_DIR. */
    if (!MakeDirPath(config_.socketPath, 0700)) {
        HILOG_ERROR(LOG_CORE, "Init: mkdir %{public}s: %{public}s",
                    config_.socketPath.c_str(), strerror(errno));
        return false;
    }
    setenv("XDG_RUNTIME_DIR", config_.socketPath.c_str(), 1);

    display_ = wl_display_create();
    if (display_ == nullptr) {
        HILOG_ERROR(LOG_CORE, "Init: wl_display_create failed");
        return false;
    }
    if (!CreateGlobals()) {
        return false;
    }
    /* eventfd is how the RS binder thread wakes this loop; adding it to
     * the wayland event loop keeps everything single-threaded above. */
    releaseEventFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (releaseEventFd_ < 0) {
        HILOG_ERROR(LOG_CORE, "Init: eventfd: %{public}s", strerror(errno));
        return false;
    }
    wl_event_loop_add_fd(wl_display_get_event_loop(display_), releaseEventFd_,
                         WL_EVENT_READABLE,
                         [](int fd, uint32_t, void* data) -> int {
                             uint64_t drained = 0;
                             (void)!read(fd, &drained, sizeof drained);
                             static_cast<Server*>(data)->DrainReleases();
                             return 0;
                         }, this);

    output_.SetReleaseCallback([this](SurfaceBuffer* buffer) {
        OnBufferReleased(buffer);
    });

    if (wl_display_add_socket(display_, config_.socketName.c_str()) < 0) {
        HILOG_ERROR(LOG_CORE, "Init: add_socket %{public}s failed",
                    config_.socketName.c_str());
        return false;
    }

    HILOG_INFO(LOG_CORE, "wayland server up: %{public}s/%{public}s, output %{public}dx%{public}d",
               config_.socketPath.c_str(), config_.socketName.c_str(),
               config_.width, config_.height);
    return true;
}

void Server::Run()
{
    running_ = true;
    struct wl_event_loop* loop = wl_display_get_event_loop(display_);
    while (running_) {
        wl_display_flush_clients(display_);
        wl_event_loop_dispatch(loop, 100);
    }
}

void Server::Stop()
{
    running_ = false;
}

} // namespace Waydroid
} // namespace OHOS
