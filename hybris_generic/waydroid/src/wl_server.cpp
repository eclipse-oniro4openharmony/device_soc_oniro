/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Globals + the per-commit frame path.  See wl_server.h for which parts
 * of the protocol we implement and why.
 */

#include "wl_server.h"

#include <cerrno>
#include <ctime>
#include <cstring>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hilog/log.h>
#include <parameter.h>

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

struct XdgSurface;

struct Surface {
    struct wl_resource* resource = nullptr;
    SurfaceState pending;
    struct wl_resource* current = nullptr;   /* committed wl_buffer */
    XdgSurface* xdg = nullptr;               /* its xdg role, if it has one */
    uint32_t toplevelId = 0;                 /* the window its frames belong to */
    bool loggedFirstBuffer = false;
    bool loggedShm = false;
};

/* xdg_surface user data.  Either side may be destroyed first (a dying client
 * tears its resources down in id order), so each clears the other's pointer. */
struct XdgSurface {
    struct wl_resource* resource = nullptr;
    Surface* surface = nullptr;
    uint32_t toplevelId = 0;
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
        /* Only two buffer factories exist here: wl_shm and android_wlegl.
         * The hwc's background/cursor surfaces commit SHM buffers, and
         * server_wlegl_buffer_from() blindly casts user_data — calling it
         * on an shm buffer is a garbage deref (first real-hwc connect
         * crashed exactly there).  wl_shm_buffer_get() is the gate: shm
         * content has no zero-copy path, so release it immediately (the
         * output node's own background is already black). */
        if (struct wl_shm_buffer* shm = wl_shm_buffer_get(surf->current)) {
            if (!surf->loggedShm) {
                surf->loggedShm = true;
                HILOG_INFO(LOG_CORE, "surface %{public}u: shm buffer %{public}dx%{public}d "
                           "(toplevel %{public}u) — not shown",
                           wl_resource_get_id(resource), wl_shm_buffer_get_width(shm),
                           wl_shm_buffer_get_height(shm), surf->toplevelId);
            }
            /* What the hwc parks on a task's surface when that task stops
             * being the one Android presents: a snapshot.  The pixels are not
             * shown (yet), but the moment is the signal. */
            server->NoteToplevelInactive(surf->toplevelId);
            wl_buffer_send_release(surf->current);
            surf->current = nullptr;
        }
    }

    if (server != nullptr && surf->current != nullptr) {
        server_wlegl_buffer* wlegl = server_wlegl_buffer_from(surf->current);
        if (wlegl != nullptr && wlegl->buf != nullptr) {
            /* A visible surface — address input to it (W4), and remember it
             * as the one its toplevel presents on. */
            server->NoteInputSurface(resource);
            server->NoteToplevelSurface(surf->toplevelId, resource);
            auto* rwb = wlegl->buf;
            if (!surf->loggedFirstBuffer) {
                surf->loggedFirstBuffer = true;
                HILOG_INFO(LOG_CORE, "surface %{public}u: first buffer %{public}dx%{public}d "
                           "fmt 0x%{public}x (toplevel %{public}u)",
                           wl_resource_get_id(resource), rwb->width, rwb->height,
                           rwb->format, surf->toplevelId);
            }
            /* Liveness for the supervisor: the container's composer got
             * through its Wayland handshake and SurfaceFlinger is
             * presenting.  A generation that never gets here is wedged
             * (seen: the composer stuck in window::create's roundtrip,
             * SurfaceFlinger waiting on IComposer forever, no tombstone)
             * and is rebuilt.  Independent of whether anything shows it. */
            static bool firstFrame = true;
            if (firstFrame) {
                firstFrame = false;
                SetParameter("waydroid.compositor.frames", "1");
                HILOG_INFO(LOG_CORE, "first frame from the container");
            }
            server->NoteToplevelFrame(surf->toplevelId);

            int32_t window = 0;
            std::shared_ptr<OutputSurface> out = server->OutputFor(surf->toplevelId, &window);
            sptr<SurfaceBuffer> sb;
            if (out != nullptr) {
                WleglBufferDesc desc;
                desc.width  = rwb->width;
                desc.height = rwb->height;
                desc.stride = rwb->stride;
                desc.format = rwb->format;
                desc.usage  = rwb->usage;
                server->WatchBufferDestroy(surf->current);
                sb = server->Importer().Import(surf->current, rwb->handle, desc);
            }
            if (sb == nullptr) {
                /* Nobody is showing this task (no window attached, or the
                 * import failed): the client owns the buffer until we say
                 * so, and nothing else ever will. */
                wl_buffer_send_release(surf->current);
            } else {
                /* The client owns the buffer until we send release, and
                 * RS is not done with it until its release listener
                 * fires — so record the mapping and let OnBufferReleased
                 * send wl_buffer.release. */
                server->TrackInFlight(sb.GetRefPtr(), surf->current, window);
                if (!out->Flush(sb, -1, rwb->width, rwb->height)) {
                    /* Almost always QUEUE_FULL: RS stopped acquiring
                     * (screen-off) and the failed flush never reaches it,
                     * so nothing ever drains the queue — a deadlock, not
                     * a hiccup (seen as one stale frame forever after an
                     * Android idle-lock).  Mailbox semantics: drop the
                     * stale queued frames, hand this output's outstanding
                     * buffers back to the hwc, retry with the newest frame. */
                    server->UntrackInFlight(sb.GetRefPtr());
                    out->ResetQueue();
                    server->ReleaseInFlightOf(window);
                    server->TrackInFlight(sb.GetRefPtr(), surf->current, window);
                    if (!out->Flush(sb, -1, rwb->width, rwb->height)) {
                        /* Nothing will ever release it: hand it straight
                         * back or the client stalls forever. */
                        server->UntrackInFlight(sb.GetRefPtr());
                        wl_buffer_send_release(surf->current);
                    }
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
    if (g_server != nullptr) {
        g_server->DropInputSurface(resource);
    }
    if (surf->xdg != nullptr) {
        surf->xdg->surface = nullptr;
    }
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
                                uint32_t id, struct wl_resource* surface,
                                struct wl_resource* parent)
{
    /* Still a stub (no positioning, no stacking) — but a subsurface's frames
     * belong to its parent's window, and whether the hwc creates any at all
     * is worth knowing. */
    auto* child = static_cast<Surface*>(wl_resource_get_user_data(surface));
    auto* par = static_cast<Surface*>(wl_resource_get_user_data(parent));
    if (child != nullptr && par != nullptr) {
        child->toplevelId = par->toplevelId;
    }
    HILOG_INFO(LOG_CORE, "subsurface: surface %{public}u under %{public}u (toplevel %{public}u)",
               wl_resource_get_id(surface), wl_resource_get_id(parent),
               par != nullptr ? par->toplevelId : 0);
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
    wl_resource_set_implementation(res, &g_outputImpl, nullptr, [](struct wl_resource* r) {
        if (g_server != nullptr) {
            g_server->RemoveOutputResource(r);
        }
    });
    if (g_server != nullptr) {
        g_server->AddOutputResource(res);
    }

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

void KeyboardRelease(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

const struct wl_keyboard_interface g_keyboardImpl = { KeyboardRelease };

/* No keymap is sent: the hwc does not interpret keys, it writes the evdev
 * code it is given into Android's input FIFO, and Android applies its own
 * key layout.  All we ever send is BACK. */
void SeatGetKeyboard(struct wl_client* client, struct wl_resource* resource, uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (res != nullptr) {
        wl_resource_set_implementation(res, &g_keyboardImpl, nullptr,
                                       [](struct wl_resource* r) {
                                           if (g_server != nullptr) {
                                               g_server->RemoveKeyboardResource(r);
                                           }
                                       });
        if (g_server != nullptr) {
            g_server->AddKeyboardResource(res);
        }
    }
}

void TouchRelease(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

const struct wl_touch_interface g_touchImpl = { TouchRelease };

void SeatGetTouch(struct wl_client* client, struct wl_resource* resource, uint32_t id)
{
    struct wl_resource* res = wl_resource_create(
        client, &wl_touch_interface, wl_resource_get_version(resource), id);
    if (res != nullptr) {
        wl_resource_set_implementation(res, &g_touchImpl, nullptr,
                                       [](struct wl_resource* r) {
                                           if (g_server != nullptr) {
                                               g_server->RemoveTouchResource(r);
                                           }
                                       });
        if (g_server != nullptr) {
            g_server->AddTouchResource(res);
        }
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

uint32_t ToplevelIdOf(struct wl_resource* resource)
{
    return static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(wl_resource_get_user_data(resource)));
}

void XdgToplevelDestroy(struct wl_client*, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
void XdgToplevelSetParent(struct wl_client*, struct wl_resource* resource,
                          struct wl_resource* parent)
{
    HILOG_INFO(LOG_CORE, "toplevel %{public}u: set_parent %{public}u", ToplevelIdOf(resource),
               parent != nullptr ? ToplevelIdOf(parent) : 0);
}
void XdgToplevelSetTitle(struct wl_client*, struct wl_resource* resource, const char* title)
{
    if (g_server != nullptr) {
        g_server->SetToplevelTitle(ToplevelIdOf(resource), title);
    }
}
void XdgToplevelSetAppId(struct wl_client*, struct wl_resource* resource, const char* appId)
{
    if (g_server != nullptr) {
        g_server->SetToplevelAppId(ToplevelIdOf(resource), appId);
    }
}
void XdgToplevelShowWindowMenu(struct wl_client*, struct wl_resource*,
                               struct wl_resource*, uint32_t, int32_t, int32_t) {}
void XdgToplevelMove(struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t) {}
void XdgToplevelResize(struct wl_client*, struct wl_resource*, struct wl_resource*,
                       uint32_t, uint32_t) {}
void XdgToplevelSetMaxSize(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}
void XdgToplevelSetMinSize(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}
void XdgToplevelSetMaximized(struct wl_client*, struct wl_resource*) {}
void XdgToplevelUnsetMaximized(struct wl_client*, struct wl_resource*) {}
void XdgToplevelSetFullscreen(struct wl_client*, struct wl_resource* resource,
                              struct wl_resource*)
{
    HILOG_INFO(LOG_CORE, "toplevel %{public}u: set_fullscreen", ToplevelIdOf(resource));
}
void XdgToplevelUnsetFullscreen(struct wl_client*, struct wl_resource*) {}
void XdgToplevelSetMinimized(struct wl_client*, struct wl_resource* resource)
{
    HILOG_INFO(LOG_CORE, "toplevel %{public}u: set_minimized", ToplevelIdOf(resource));
}

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
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(resource));
    uint32_t tid = g_server->AddToplevel(res, resource);
    if (xdg != nullptr && xdg->surface != nullptr) {
        g_server->NoteToplevelSurface(tid, xdg->surface->resource);
    }
    if (xdg != nullptr) {
        xdg->toplevelId = tid;
        if (xdg->surface != nullptr) {
            xdg->surface->toplevelId = tid;
        }
    }
    wl_resource_set_implementation(
        res, &g_xdgToplevelImpl, reinterpret_cast<void*>(static_cast<uintptr_t>(tid)),
        [](struct wl_resource* r) {
            if (g_server != nullptr) {
                g_server->RemoveToplevel(ToplevelIdOf(r));
            }
        });

    /* Tell the container it is fullscreen, at the size of the window it is
     * shown in, right away — the hwc waits for this configure before it
     * commits anything, and sizes Android's display to it. */
    struct wl_array states;
    wl_array_init(&states);
    if (uint32_t* s = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)))) {
        *s = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    /* Not ACTIVATED yet: which task this is only shows with set_app_id, and
     * activation must single out one window (Server::ApplyActivation). */
    int32_t w = 0;
    int32_t h = 0;
    g_server->OutputSize(&w, &h);
    xdg_toplevel_send_configure(res, w, h, &states);
    wl_array_release(&states);

    xdg_surface_send_configure(resource, wl_display_next_serial(g_server->Display()));
}

void XdgSurfaceGetPopup(struct wl_client*, struct wl_resource*, uint32_t,
                        struct wl_resource*, struct wl_resource*) {}
void XdgSurfaceSetWindowGeometry(struct wl_client*, struct wl_resource* resource,
                                 int32_t x, int32_t y, int32_t w, int32_t h)
{
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(resource));
    HILOG_INFO(LOG_CORE, "toplevel %{public}u: set_window_geometry %{public}d,%{public}d "
               "%{public}dx%{public}d", xdg != nullptr ? xdg->toplevelId : 0, x, y, w, h);
}
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
                            uint32_t id, struct wl_resource* surface)
{
    struct wl_resource* res = wl_resource_create(
        client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* xdg = new XdgSurface();
    xdg->resource = res;
    xdg->surface = static_cast<Surface*>(wl_resource_get_user_data(surface));
    if (xdg->surface != nullptr) {
        xdg->surface->xdg = xdg;
    }
    wl_resource_set_implementation(res, &g_xdgSurfaceImpl, xdg, [](struct wl_resource* r) {
        auto* x = static_cast<XdgSurface*>(wl_resource_get_user_data(r));
        if (x->surface != nullptr) {
            x->surface->xdg = nullptr;
        }
        delete x;
    });
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
    if (touchEventFd_ >= 0) {
        close(touchEventFd_);
        touchEventFd_ = -1;
    }
    if (postedEventFd_ >= 0) {
        close(postedEventFd_);
        postedEventFd_ = -1;
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
            it = (it->second.wlBuffer == wlBuffer) ? inFlight_.erase(it) : std::next(it);
        }
        for (auto it = releasedPending_.begin(); it != releasedPending_.end();) {
            it = (*it == wlBuffer) ? releasedPending_.erase(it) : std::next(it);
        }
    }
    /* The queues still hold slots for this client's buffers and nothing
     * will release them now — start over. */
    output_->ResetQueue();
    std::vector<std::shared_ptr<Window>> windows;
    {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        for (auto& kv : windows_) {
            windows.push_back(kv.second);
        }
    }
    for (auto& w : windows) {
        w->output->ResetQueue();
    }
}

/* ---- deferred wl_buffer.release ---------------------------------------- */

void Server::TrackInFlight(SurfaceBuffer* buffer, struct wl_resource* wlBuffer, int32_t window)
{
    std::lock_guard<std::mutex> lock(inFlightMutex_);
    inFlight_[buffer] = InFlight { wlBuffer, window };
}

void Server::UntrackInFlight(SurfaceBuffer* buffer)
{
    std::lock_guard<std::mutex> lock(inFlightMutex_);
    inFlight_.erase(buffer);
}

/* Wayland thread. */
void Server::ReleaseEverythingInFlight()
{
    std::vector<struct wl_resource*> resources;
    {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        for (auto& kv : inFlight_) {
            resources.push_back(kv.second.wlBuffer);
        }
        inFlight_.clear();
        releasedPending_.clear();
    }
    for (struct wl_resource* res : resources) {
        wl_buffer_send_release(res);
    }
    if (!resources.empty()) {
        wl_display_flush_clients(display_);
    }
}

/* Wayland thread.  One output's share of the above: its queue was reset or
 * it is going away, so nothing will release what it still holds. */
void Server::ReleaseInFlightOf(int32_t window)
{
    std::vector<struct wl_resource*> resources;
    {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        for (auto it = inFlight_.begin(); it != inFlight_.end();) {
            if (it->second.window == window) {
                resources.push_back(it->second.wlBuffer);
                it = inFlight_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (struct wl_resource* res : resources) {
        wl_buffer_send_release(res);
    }
    if (!resources.empty()) {
        wl_display_flush_clients(display_);
    }
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
        releasedPending_.push_back(it->second.wlBuffer);
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

/* ---- touch input (W4) --------------------------------------------------- */

/* Any thread. */
void Server::InjectTouchOps(const std::vector<TouchOp>& ops)
{
    if (ops.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(touchMutex_);
        touchPending_.insert(touchPending_.end(), ops.begin(), ops.end());
    }
    if (touchEventFd_ >= 0) {
        uint64_t one = 1;
        (void)!write(touchEventFd_, &one, sizeof one);
    }
}

/* Any thread (W5 IPC).  OHOS PointerEvent action codes. */
void Server::InjectTouchFromAction(int32_t action, int32_t id, int32_t x, int32_t y)
{
    constexpr int32_t ACTION_CANCEL = 1;
    constexpr int32_t ACTION_DOWN = 2;
    constexpr int32_t ACTION_MOVE = 3;
    constexpr int32_t ACTION_UP = 4;

    std::vector<TouchOp> ops;
    switch (action) {
        case ACTION_DOWN:   ops.push_back({ TouchOp::Down, id, x, y }); break;
        case ACTION_MOVE:   ops.push_back({ TouchOp::Motion, id, x, y }); break;
        case ACTION_UP:     ops.push_back({ TouchOp::Up, id, x, y }); break;
        case ACTION_CANCEL: ops.push_back({ TouchOp::Cancel, id, x, y }); break;
        default: return;
    }
    ops.push_back({ TouchOp::Frame, 0, 0, 0 });
    InjectTouchOps(ops);
}

/* Wayland thread. */
void Server::AddTouchResource(struct wl_resource* touch)
{
    touchResources_.push_back(touch);
}

void Server::RemoveTouchResource(struct wl_resource* touch)
{
    for (auto it = touchResources_.begin(); it != touchResources_.end();) {
        it = (*it == touch) ? touchResources_.erase(it) : std::next(it);
    }
}

void Server::NoteInputSurface(struct wl_resource* surface)
{
    inputSurface_ = surface;
}

void Server::DropInputSurface(struct wl_resource* surface)
{
    if (inputSurface_ == surface) {
        inputSurface_ = nullptr;
    }
    if (keyboardFocus_ == surface) {
        keyboardFocus_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(toplevelMutex_);
    for (auto& kv : toplevels_) {
        if (kv.second.surface == surface) {
            kv.second.surface = nullptr;
            kv.second.entered = false;
        }
    }
}

/* Wayland thread (called from the W5 IPC binder thread is NOT safe for
 * RS node ops — but AttachSelfDrawingNode takes its own lock and only
 * touches RS, not wl_*; RS transactions are thread-safe).  Reverts the
 * output to the built-in self-drawing node at the configured geometry. */
void Server::RevertToSelfDrawing()
{
    output_->AttachSelfDrawingNode(config_.screenId, config_.width, config_.height);
}

/* Wayland thread: replay queued ops as wl_touch events on every touch
 * resource of the input surface's client (one hwc in practice).  The
 * hwc turns them into evdev packets on its container-internal FIFO
 * (wayland-hwc.cpp touch_handle_*), InputFlinger takes it from there. */
void Server::DrainTouchOps()
{
    std::vector<TouchOp> ops;
    {
        std::lock_guard<std::mutex> lock(touchMutex_);
        ops.swap(touchPending_);
    }
    if (ops.empty()) {
        return;
    }
    if (inputSurface_ == nullptr || touchResources_.empty()) {
        HILOG_INFO(LOG_CORE, "DrainTouchOps: %{public}zu ops but inputSurface=%{public}d "
                   "touchResources=%{public}zu (dropped)",
                   ops.size(), inputSurface_ != nullptr, touchResources_.size());
        return;
    }
    struct wl_client* owner = wl_resource_get_client(inputSurface_);

    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t ms = static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

    bool sent = false;
    for (const TouchOp& op : ops) {
        for (struct wl_resource* tr : touchResources_) {
            if (wl_resource_get_client(tr) != owner) {
                continue;
            }
            switch (op.kind) {
                case TouchOp::Down:
                    wl_touch_send_down(tr, wl_display_next_serial(display_),
                                       ms, inputSurface_, op.id,
                                       wl_fixed_from_int(op.x),
                                       wl_fixed_from_int(op.y));
                    break;
                case TouchOp::Up:
                    wl_touch_send_up(tr, wl_display_next_serial(display_),
                                     ms, op.id);
                    break;
                case TouchOp::Motion:
                    wl_touch_send_motion(tr, ms, op.id,
                                         wl_fixed_from_int(op.x),
                                         wl_fixed_from_int(op.y));
                    break;
                case TouchOp::Frame:
                    wl_touch_send_frame(tr);
                    break;
                case TouchOp::Cancel:
                    wl_touch_send_cancel(tr);
                    break;
                default:
                    break;
            }
            sent = true;
        }
    }
    if (sent) {
        wl_display_flush_clients(display_);
        HILOG_DEBUG(LOG_CORE, "DrainTouchOps: sent %{public}zu ops to %{public}zu touch res",
                    ops.size(), touchResources_.size());
    }
}

/* ---- posted work ---------------------------------------------------------- */

/* Any thread. */
void Server::Post(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lock(postedMutex_);
        posted_.push_back(std::move(fn));
    }
    if (postedEventFd_ >= 0) {
        uint64_t one = 1;
        (void)!write(postedEventFd_, &one, sizeof one);
    }
}

/* Wayland thread. */
void Server::DrainPosted()
{
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(postedMutex_);
        work.swap(posted_);
    }
    for (auto& fn : work) {
        fn();
    }
    if (!work.empty()) {
        wl_display_flush_clients(display_);
    }
}

/* ---- toplevel table --------------------------------------------------------- */

namespace {
constexpr const char* kAppIdPrefix = "waydroid.";

/* "waydroid.<package>" → "<package>"; anything else (the full-UI toplevel's
 * "Waydroid") has no package. */
std::string PackageOfAppId(const std::string& appId)
{
    const size_t n = strlen(kAppIdPrefix);
    return appId.compare(0, n, kAppIdPrefix) == 0 ? appId.substr(n) : std::string();
}

} // namespace

/*
 * Android's HOME.  There always is one: LineageOS' setup wizard until it has
 * been clicked through (it holds the HOME role with priority, and marking
 * Android provisioned does not retire it), launcher3 after that.  In the
 * hwc's per-task mode it gets a window like any other task.  We never show
 * it — the OHOS desktop is the desktop — and never close it either: Android
 * restarts a removed HOME at once, and closing the hwc's LAST window makes it
 * reconnect, so closing on sight is an endless loop (tried).  What it is good
 * for: HOME coming to the top means the user backed out of the app's last
 * activity, which is the shell's cue to close the OHOS window ("home").
 */
bool IsHomePackage(const std::string& package)
{
    return package == "org.lineageos.setupwizard" || package == "com.android.launcher3";
}

namespace {
} // namespace

uint32_t Server::NoteClientConnected()
{
    return ++clients_;
}

void Server::EmitToplevel(ToplevelEvent ev, const ToplevelInfo& info)
{
    static const char* const names[] = { "created", "updated", "on-top", "inactive", "destroyed" };
    HILOG_INFO(LOG_CORE, "toplevel %{public}u %{public}s: client %{public}u app_id '%{public}s' "
               "title '%{public}s' mapped %{public}d frames %{public}llu",
               info.id, names[static_cast<int32_t>(ev)], info.client, info.appId.c_str(),
               info.title.c_str(), info.mapped,
               static_cast<unsigned long long>(info.frames));
    if (toplevelCb_) {
        toplevelCb_(ev, info);
    }
}

uint32_t Server::AddToplevel(struct wl_resource* toplevel, struct wl_resource* xdgSurface)
{
    ToplevelRec rec;
    rec.toplevel = toplevel;
    rec.xdgSurface = xdgSurface;
    rec.info.client = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
        wl_client_get_user_data(wl_resource_get_client(toplevel))));
    ToplevelInfo info;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        rec.info.id = nextToplevelId_++;
        info = rec.info;
        toplevels_[info.id] = rec;
    }
    EmitToplevel(ToplevelEvent::Created, info);
    return info.id;
}

void Server::RemoveToplevel(uint32_t id)
{
    ToplevelInfo info;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        auto it = toplevels_.find(id);
        if (it == toplevels_.end()) {
            return;
        }
        info = it->second.info;
        toplevels_.erase(it);
    }
    if (onTopId_ == id) {
        onTopId_ = 0;
    }
    EmitToplevel(ToplevelEvent::Destroyed, info);
}

void Server::SetToplevelAppId(uint32_t id, const char* appId)
{
    ToplevelInfo info;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        auto it = toplevels_.find(id);
        if (it == toplevels_.end() || appId == nullptr || it->second.info.appId == appId) {
            return;
        }
        it->second.info.appId = appId;
        it->second.home = IsHomePackage(PackageOfAppId(appId));
        info = it->second.info;
    }
    EmitToplevel(ToplevelEvent::Updated, info);
    ApplyActivation(false);
}

void Server::SetToplevelTitle(uint32_t id, const char* title)
{
    ToplevelInfo info;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        auto it = toplevels_.find(id);
        if (it == toplevels_.end() || title == nullptr || it->second.info.title == title) {
            return;
        }
        it->second.info.title = title;
        info = it->second.info;
    }
    EmitToplevel(ToplevelEvent::Updated, info);
}

/* Per frame — keep it cheap: one lock, and an event only when the window
 * that presents changes. */
void Server::NoteToplevelFrame(uint32_t id)
{
    ToplevelInfo info;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        auto it = toplevels_.find(id);
        if (it == toplevels_.end()) {
            return;
        }
        it->second.info.frames++;
        it->second.info.mapped = true;
        if (onTopId_ == id) {
            return;
        }
        onTopId_ = id;
        info = it->second.info;
    }
    EmitToplevel(ToplevelEvent::OnTop, info);
}

/* Wayland thread.  The task went to the back inside Android — the user
 * backed out of its last activity, or another task took over. */
void Server::NoteToplevelInactive(uint32_t id)
{
    ToplevelInfo info;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        auto it = toplevels_.find(id);
        if (it == toplevels_.end() || onTopId_ != id) {
            return;
        }
        onTopId_ = 0;               /* its next real frame is an OnTop again */
        info = it->second.info;
    }
    EmitToplevel(ToplevelEvent::Inactive, info);
}

std::vector<ToplevelInfo> Server::Toplevels()
{
    std::vector<ToplevelInfo> out;
    std::lock_guard<std::mutex> lock(toplevelMutex_);
    out.reserve(toplevels_.size());
    for (auto& kv : toplevels_) {
        out.push_back(kv.second.info);
    }
    return out;
}

void Server::CloseToplevel(uint32_t id)
{
    struct wl_resource* res = nullptr;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        auto it = toplevels_.find(id);
        if (it != toplevels_.end()) {
            res = it->second.toplevel;
        }
    }
    if (res != nullptr) {
        HILOG_INFO(LOG_CORE, "toplevel %{public}u: sending close", id);
        xdg_toplevel_send_close(res);
    }
}

void Server::ConfigureToplevel(uint32_t id, int32_t width, int32_t height, bool activated)
{
    std::vector<ToplevelRec> targets;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        for (auto& kv : toplevels_) {
            if (id == 0 || kv.first == id) {
                targets.push_back(kv.second);
            }
        }
    }
    for (auto& rec : targets) {
        struct wl_array states;
        wl_array_init(&states);
        if (uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)))) {
            *st = XDG_TOPLEVEL_STATE_FULLSCREEN;
        }
        if (activated) {
            if (uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)))) {
                *st = XDG_TOPLEVEL_STATE_ACTIVATED;
            }
        }
        {
            std::lock_guard<std::mutex> lock(toplevelMutex_);
            auto it = toplevels_.find(rec.info.id);
            if (it != toplevels_.end()) {
                it->second.activated = activated;
            }
        }
        HILOG_INFO(LOG_CORE, "toplevel %{public}u: configure %{public}dx%{public}d activated %{public}d",
                   rec.info.id, width, height, activated);
        xdg_toplevel_send_configure(rec.toplevel, width, height, &states);
        wl_array_release(&states);
        xdg_surface_send_configure(rec.xdgSurface, wl_display_next_serial(display_));
    }
}

/* ---- windows ------------------------------------------------------------------ */

namespace {
class ProducerDeathRecipient : public IRemoteObject::DeathRecipient {
public:
    ProducerDeathRecipient(Server* server, int32_t window) : server_(server), window_(window) {}
    void OnRemoteDied(const wptr<IRemoteObject>&) override
    {
        HILOG_WARN(LOG_CORE, "window %{public}d: its producer died (shell gone); detaching",
                   window_);
        server_->DetachWindow(window_);
    }

private:
    Server* server_;
    int32_t window_;
};
} // namespace

void Server::OutputSize(int32_t* width, int32_t* height)
{
    std::lock_guard<std::mutex> lock(windowsMutex_);
    *width = outputWidth_;
    *height = outputHeight_;
}

/* Any thread. */
int32_t Server::AttachWindow(const std::string& package, const sptr<IBufferProducer>& producer,
                             int32_t width, int32_t height)
{
    if (producer == nullptr || width <= 0 || height <= 0) {
        return -1;
    }
    auto window = std::make_shared<Window>();
    window->package = package;
    window->output = std::make_shared<OutputSurface>();
    window->output->SetReleaseCallback([this](SurfaceBuffer* buffer) {
        OnBufferReleased(buffer);
    });
    if (!window->output->AttachProducer(producer)) {
        return -1;
    }
    bool resized = false;
    {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        window->id = nextWindowId_++;
        windows_[window->id] = window;
        resized = (outputWidth_ != width || outputHeight_ != height);
        outputWidth_ = width;
        outputHeight_ = height;
    }
    /* If the shell dies its surface dies with it; without this the window
     * (and the Android buffers parked in its queue) would stay forever. */
    window->producerObject = producer->AsObject();
    if (window->producerObject != nullptr) {
        window->death = new ProducerDeathRecipient(this, window->id);
        window->producerObject->AddDeathRecipient(window->death);
    }
    HILOG_INFO(LOG_CORE, "window %{public}d attached: package '%{public}s' %{public}dx%{public}d",
               window->id, package.c_str(), width, height);
    Post([this, width, height, resized]() {
        if (resized) {
            /* Android's display becomes the window's content area.  Every
             * toplevel gets the size — the hwc follows the LAST configure it
             * saw from any of them — and keeps its own activation. */
            std::vector<std::pair<uint32_t, bool>> all;
            {
                std::lock_guard<std::mutex> lock(toplevelMutex_);
                for (auto& kv : toplevels_) {
                    all.emplace_back(kv.first, kv.second.activated);
                }
            }
            for (auto& t : all) {
                ConfigureToplevel(t.first, width, height, t.second);
            }
        }
        /* Activation — and with it a first frame — follows the shell's
         * SetWindowActive, which it calls as soon as the window is in front. */
    });
    return window->id;
}

/* Any thread. */
bool Server::DetachWindow(int32_t id)
{
    std::shared_ptr<Window> window;
    {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) {
            return false;
        }
        window = it->second;
        windows_.erase(it);
    }
    if (window->producerObject != nullptr && window->death != nullptr) {
        window->producerObject->RemoveDeathRecipient(window->death);
    }
    {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        if (activeWindow_ == id) {
            activeWindow_ = 0;
            activePackage_.clear();
        }
    }
    HILOG_INFO(LOG_CORE, "window %{public}d detached", id);
    /* The output's queue dies with the shell's surface; the Android buffers
     * still in it must go back to the hwc or SurfaceFlinger runs dry.  The
     * shared_ptr keeps the output alive until the wayland thread is done. */
    Post([this, window]() {
        window->output->Detach();
        ReleaseInFlightOf(window->id);
        ApplyActivation(false);
    });
    return true;
}

bool Server::WindowAlive(int32_t id)
{
    std::lock_guard<std::mutex> lock(windowsMutex_);
    return windows_.count(id) != 0;
}

/* Wayland thread. */
std::shared_ptr<OutputSurface> Server::OutputFor(uint32_t toplevelId, int32_t* windowOut)
{
    std::string package;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        auto it = toplevels_.find(toplevelId);
        if (it != toplevels_.end()) {
            if (it->second.home) {
                *windowOut = 0;
                return nullptr;         /* never shown; see IsHomePackage */
            }
            package = PackageOfAppId(it->second.info.appId);
        }
    }
    std::lock_guard<std::mutex> lock(windowsMutex_);
    std::shared_ptr<Window> any;
    for (auto& kv : windows_) {
        if (!package.empty() && kv.second->package == package) {
            kv.second->toplevelId = toplevelId;
            *windowOut = kv.first;
            return kv.second->output;
        }
        if (kv.second->package.empty()) {
            any = kv.second;
        }
    }
    if (any != nullptr) {
        any->toplevelId = toplevelId;
        *windowOut = any->id;
        return any->output;
    }
    *windowOut = 0;
    return output_->IsAttached() ? output_ : nullptr;
}

/* Wayland thread. */
void Server::NoteToplevelSurface(uint32_t toplevelId, struct wl_resource* surface)
{
    std::lock_guard<std::mutex> lock(toplevelMutex_);
    auto it = toplevels_.find(toplevelId);
    if (it != toplevels_.end()) {
        it->second.surface = surface;
    }
}

/* Any thread.  The surface a window's touches go to is the one its toplevel
 * presents on — the hwc uses it to tell which Android task was touched. */
void Server::WindowTouch(int32_t window, int32_t action, int32_t id, int32_t x, int32_t y)
{
    Post([this, window, action, id, x, y]() {
        uint32_t toplevelId = 0;
        {
            std::lock_guard<std::mutex> lock(windowsMutex_);
            auto it = windows_.find(window);
            if (it == windows_.end()) {
                return;
            }
            toplevelId = it->second->toplevelId;
        }
        {
            std::lock_guard<std::mutex> lock(toplevelMutex_);
            auto it = toplevels_.find(toplevelId);
            if (it != toplevels_.end() && it->second.surface != nullptr) {
                inputSurface_ = it->second.surface;
            }
        }
        InjectTouchFromAction(action, id, x, y);
        DrainTouchOps();
    });
}

void Server::AddOutputResource(struct wl_resource* output)
{
    outputResources_.push_back(output);
}

void Server::RemoveOutputResource(struct wl_resource* output)
{
    for (auto it = outputResources_.begin(); it != outputResources_.end();) {
        it = (*it == output) ? outputResources_.erase(it) : std::next(it);
    }
}

/*
 * wl_surface.enter / leave: "this surface is (not) on an output".  This hwc
 * decides from it whether the host is showing a window at all — with no
 * window on any output it turns Android's display off, and an activation
 * only wakes it for an instant.  A compositor that never sends enter (ours,
 * until two windows were open at once) gets frames only by that accident.
 * Wayland thread.
 */
void Server::SendSurfaceOnOutput(uint32_t toplevelId, bool shown)
{
    struct wl_resource* surface = nullptr;
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        auto it = toplevels_.find(toplevelId);
        if (it == toplevels_.end() || it->second.surface == nullptr ||
            it->second.entered == shown) {
            return;
        }
        it->second.entered = shown;
        surface = it->second.surface;
    }
    struct wl_client* owner = wl_resource_get_client(surface);
    for (struct wl_resource* out : outputResources_) {
        if (wl_resource_get_client(out) != owner) {
            continue;
        }
        if (shown) {
            wl_surface_send_enter(surface, out);
        } else {
            wl_surface_send_leave(surface, out);
        }
    }
}

void Server::AddKeyboardResource(struct wl_resource* keyboard)
{
    keyboardResources_.push_back(keyboard);
}

void Server::RemoveKeyboardResource(struct wl_resource* keyboard)
{
    for (auto it = keyboardResources_.begin(); it != keyboardResources_.end();) {
        it = (*it == keyboard) ? keyboardResources_.erase(it) : std::next(it);
    }
}

/* Wayland thread.  wl_keyboard.key is only valid on a surface that has
 * keyboard focus, so enter it first (once per surface). */
void Server::SendKeyboardFocus(struct wl_resource* surface)
{
    if (surface == nullptr || surface == keyboardFocus_) {
        return;
    }
    struct wl_client* owner = wl_resource_get_client(surface);
    struct wl_array keys;
    wl_array_init(&keys);
    for (struct wl_resource* kb : keyboardResources_) {
        if (wl_resource_get_client(kb) != owner) {
            continue;
        }
        if (keyboardFocus_ != nullptr) {
            wl_keyboard_send_leave(kb, wl_display_next_serial(display_), keyboardFocus_);
        }
        wl_keyboard_send_enter(kb, wl_display_next_serial(display_), surface, &keys);
        wl_keyboard_send_modifiers(kb, wl_display_next_serial(display_), 0, 0, 0, 0);
    }
    wl_array_release(&keys);
    keyboardFocus_ = surface;
}

/* Any thread. */
void Server::WindowKey(int32_t window, int32_t code, bool down)
{
    Post([this, window, code, down]() {
        uint32_t toplevelId = 0;
        {
            std::lock_guard<std::mutex> lock(windowsMutex_);
            auto it = windows_.find(window);
            if (it == windows_.end()) {
                return;
            }
            toplevelId = it->second->toplevelId;
        }
        struct wl_resource* surface = nullptr;
        {
            std::lock_guard<std::mutex> lock(toplevelMutex_);
            auto it = toplevels_.find(toplevelId);
            if (it != toplevels_.end()) {
                surface = it->second.surface;
            }
        }
        if (surface == nullptr) {
            surface = inputSurface_;
        }
        if (surface == nullptr || keyboardResources_.empty()) {
            HILOG_WARN(LOG_CORE, "key %{public}d dropped: surface %{public}d keyboards %{public}zu",
                       code, surface != nullptr, keyboardResources_.size());
            return;
        }
        SendKeyboardFocus(surface);
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t ms = static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        struct wl_client* owner = wl_resource_get_client(surface);
        for (struct wl_resource* kb : keyboardResources_) {
            if (wl_resource_get_client(kb) == owner) {
                wl_keyboard_send_key(kb, wl_display_next_serial(display_), ms,
                                     static_cast<uint32_t>(code),
                                     down ? WL_KEYBOARD_KEY_STATE_PRESSED
                                          : WL_KEYBOARD_KEY_STATE_RELEASED);
            }
        }
        HILOG_INFO(LOG_CORE, "window %{public}d: key %{public}d %{public}s", window, code,
                   down ? "down" : "up");
    });
}

/*
 * Activation mirrors OHOS: the one toplevel whose OHOS window is in front is
 * ACTIVATED, every other task window is not.  It matters more than it looks:
 * this hwc takes the activation edge as "the user went to this window" and
 * makes that Android's focused task (setFocusedTask), presents live frames
 * only on the window it believes focused — the others get a parked snapshot —
 * and lets Android's display sleep when nothing is activated.  Activating
 * everything (what a single-window compositor naturally does) leaves it
 * posting snapshots to the window the user is looking at.
 *
 * The full-UI toplevel ("Waydroid", no package) is always activated: it is
 * the whole desktop, shown by the debug overlay and watched by the
 * supervisor's first-frame check.
 *
 * Wayland thread.  `edge`: deactivate-then-activate the active one even if it
 * already is, to make the hwc present again (a window re-attached to a task
 * that kept running would otherwise wait for Android to draw something).
 */
void Server::ApplyActivation(bool edge)
{
    std::string active;
    int32_t w = 0;
    int32_t h = 0;
    {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        active = activePackage_;
        w = outputWidth_;
        h = outputHeight_;
    }
    if (output_->IsAttached()) {
        active = "*";                   /* debug overlay / bring-up tool: show it all */
    }
    std::vector<std::pair<uint32_t, bool>> changes;
    std::vector<uint32_t> pulse;        /* already active: off-then-on */
    {
        std::lock_guard<std::mutex> lock(toplevelMutex_);
        for (auto& kv : toplevels_) {
            ToplevelRec& rec = kv.second;
            const std::string package = PackageOfAppId(rec.info.appId);
            bool want = true;
            if (!package.empty()) {
                want = !rec.home && (active == "*" || package == active);
            } else if (rec.info.appId.empty()) {
                continue;               /* not named yet */
            }
            if (want && edge && rec.activated && !package.empty()) {
                pulse.push_back(kv.first);
            } else if (want != rec.activated) {
                rec.activated = want;
                changes.emplace_back(kv.first, want);
            }
        }
    }
    /* The window in front first — on an output, then activated — and only
     * then the others off: at no instant is there no shown, active window
     * (the hwc would put Android's display to sleep in between). */
    for (auto& c : changes) {
        if (c.second) {
            SendSurfaceOnOutput(c.first, true);
            ConfigureToplevel(c.first, w, h, true);
        }
    }
    for (auto& c : changes) {
        if (!c.second) {
            ConfigureToplevel(c.first, w, h, false);
            SendSurfaceOnOutput(c.first, false);
        }
    }
    for (uint32_t id : pulse) {
        SendSurfaceOnOutput(id, true);
        ConfigureToplevel(id, w, h, false);
        ConfigureToplevel(id, w, h, true);
    }
}

/* Any thread. */
void Server::SetWindowActive(int32_t window, bool active)
{
    {
        std::lock_guard<std::mutex> lock(windowsMutex_);
        auto it = windows_.find(window);
        if (it == windows_.end()) {
            return;
        }
        it->second->active = active;
        if (active) {
            activeWindow_ = window;
            activePackage_ = it->second->package.empty() ? "*" : it->second->package;
        } else if (activeWindow_ == window) {
            activeWindow_ = 0;
            activePackage_.clear();
        }
    }
    HILOG_INFO(LOG_CORE, "window %{public}d %{public}s", window, active ? "active" : "inactive");
    Post([this, active]() {
        onTopId_ = 0;                   /* re-arm: the next frame is an OnTop */
        ApplyActivation(active);
    });
}

/* Any thread. */
void Server::CloseApp(const std::string& package)
{
    Post([this, package]() {
        std::vector<uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(toplevelMutex_);
            for (auto& kv : toplevels_) {
                if (PackageOfAppId(kv.second.info.appId) == package) {
                    ids.push_back(kv.first);
                }
            }
        }
        for (uint32_t id : ids) {
            CloseToplevel(id);
        }
    });
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
     * /run/xdg; libwayland takes it from XDG_RUNTIME_DIR.  We run as
     * root but the container's hwcomposer runs as system (uid 1000), and
     * connect(2) on a unix socket needs search on the dir and write on
     * the socket inode — hence the explicit chmods (mkdir alone is
     * umask-masked and never fixes a dir that already exists). */
    if (!MakeDirPath(config_.socketPath, 0755)) {
        HILOG_ERROR(LOG_CORE, "Init: mkdir %{public}s: %{public}s",
                    config_.socketPath.c_str(), strerror(errno));
        return false;
    }
    (void)chmod(config_.socketPath.c_str(), 0755);
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

    /* Same wake-the-loop pattern for touch ops from the MMI thread. */
    touchEventFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (touchEventFd_ < 0) {
        HILOG_ERROR(LOG_CORE, "Init: touch eventfd: %{public}s", strerror(errno));
        return false;
    }
    wl_event_loop_add_fd(wl_display_get_event_loop(display_), touchEventFd_,
                         WL_EVENT_READABLE,
                         [](int fd, uint32_t, void* data) -> int {
                             uint64_t drained = 0;
                             (void)!read(fd, &drained, sizeof drained);
                             static_cast<Server*>(data)->DrainTouchOps();
                             return 0;
                         }, this);

    postedEventFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (postedEventFd_ < 0) {
        HILOG_ERROR(LOG_CORE, "Init: posted eventfd: %{public}s", strerror(errno));
        return false;
    }
    wl_event_loop_add_fd(wl_display_get_event_loop(display_), postedEventFd_,
                         WL_EVENT_READABLE,
                         [](int fd, uint32_t, void* data) -> int {
                             uint64_t drained = 0;
                             (void)!read(fd, &drained, sizeof drained);
                             static_cast<Server*>(data)->DrainPosted();
                             return 0;
                         }, this);

    /* Number the connections.  Upstream's hwc is one client for the life of
     * a container; Volla's A16 one reconnects when its last window is closed
     * and can open a connection per Android task, so "which client" is part
     * of a window's identity. */
    static struct wl_listener clientCreated;
    clientCreated.notify = [](struct wl_listener*, void* data) {
        auto* client = static_cast<struct wl_client*>(data);
        uint32_t n = g_server != nullptr ? g_server->NoteClientConnected() : 0;
        wl_client_set_user_data(client, reinterpret_cast<void*>(static_cast<uintptr_t>(n)), nullptr);
        pid_t pid = 0;
        wl_client_get_credentials(client, &pid, nullptr, nullptr);
        HILOG_INFO(LOG_CORE, "client %{public}u connected (host pid %{public}d)", n, pid);
        auto* gone = new struct wl_listener();
        gone->notify = [](struct wl_listener* l, void* d) {
            auto* c = static_cast<struct wl_client*>(d);
            HILOG_INFO(LOG_CORE, "client %{public}u disconnected",
                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wl_client_get_user_data(c))));
            wl_list_remove(&l->link);
            delete l;
        };
        wl_client_add_destroy_listener(client, gone);
    };
    wl_display_add_client_created_listener(display_, &clientCreated);

    output_->SetReleaseCallback([this](SurfaceBuffer* buffer) {
        OnBufferReleased(buffer);
    });
    outputWidth_ = config_.width;
    outputHeight_ = config_.height;

    if (wl_display_add_socket(display_, config_.socketName.c_str()) < 0) {
        HILOG_ERROR(LOG_CORE, "Init: add_socket %{public}s failed",
                    config_.socketName.c_str());
        return false;
    }
    (void)chmod((config_.socketPath + "/" + config_.socketName).c_str(), 0666);

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
        if (tick_) {
            tick_();
        }
    }
}

void Server::Stop()
{
    running_ = false;
}

} // namespace Waydroid
} // namespace OHOS
