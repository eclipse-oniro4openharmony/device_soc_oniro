/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * wlegl_harness — stands in for the container's hwcomposer.
 *
 * Allocates real gralloc buffers through libhybris, fills them on the
 * CPU with a moving pattern, and posts them to waydroid_compositor over
 * `android_wlegl` exactly as the Waydroid hwc does. That exercises the
 * whole W2 frame path — handle marshalling, server-side import,
 * BufferHandle construction, SurfaceBuffer adoption, attach+flush,
 * release — with no container in the picture.
 *
 * Usage: waydroid_wlegl_harness [frames] [width] [height] [usage_hex]
 * A frame count of 0 runs until killed (the soak configuration:
 * watch RSS and /proc/<pid>/fd for leaks).  usage_hex overrides the
 * gralloc usage (default 0x933) — used to probe whether libhybris'
 * hybris_gralloc_allocate accepts SurfaceFlinger's usage combos
 * (e.g. 0x1b00 = HW_TEXTURE|HW_RENDER|HW_COMPOSER|HW_FB) that the
 * HIDL/AIDL gralloc4 path rejects (W3 gralloc-ABI-skew de-risk).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <unistd.h>

#include <wayland-client.h>
#include "wayland-android-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <hybris/gralloc/gralloc.h>
#include <cutils/native_handle.h>

namespace {

constexpr int32_t HAL_RGBA_8888 = 1;
/* GRALLOC_USAGE_SW_{READ,WRITE}_OFTEN | HW_TEXTURE | HW_COMPOSER */
constexpr int32_t HARNESS_USAGE = 0x00000003 | 0x00000030 | 0x00000100 | 0x00000800;

struct Globals {
    struct wl_compositor* compositor = nullptr;
    struct xdg_wm_base* wmBase = nullptr;
    struct android_wlegl* wlegl = nullptr;
};

void RegistryGlobal(void* data, struct wl_registry* registry, uint32_t name,
                    const char* iface, uint32_t version)
{
    auto* g = static_cast<Globals*>(data);
    if (strcmp(iface, "wl_compositor") == 0) {
        g->compositor = static_cast<struct wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             version < 4 ? version : 4));
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        g->wmBase = static_cast<struct xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    } else if (strcmp(iface, "android_wlegl") == 0) {
        g->wlegl = static_cast<struct android_wlegl*>(
            wl_registry_bind(registry, name, &android_wlegl_interface, 1));
    }
}

void RegistryGlobalRemove(void*, struct wl_registry*, uint32_t) {}

const struct wl_registry_listener g_registryListener = {
    RegistryGlobal, RegistryGlobalRemove,
};

int g_released = 0;
/* Per-buffer "the server still has this" flag: a client may not touch a
 * buffer between attach and wl_buffer.release. */
bool g_busy[3] = { false, false, false };

void BufferRelease(void* data, struct wl_buffer*)
{
    ++g_released;
    auto* busy = static_cast<bool*>(data);
    if (busy != nullptr) {
        *busy = false;
    }
}

const struct wl_buffer_listener g_bufferListener = { BufferRelease };

void XdgSurfaceConfigure(void*, struct xdg_surface* surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
}

const struct xdg_surface_listener g_xdgSurfaceListener = { XdgSurfaceConfigure };

void XdgToplevelConfigure(void*, struct xdg_toplevel*, int32_t, int32_t,
                          struct wl_array*) {}
void XdgToplevelClose(void*, struct xdg_toplevel*) {}

const struct xdg_toplevel_listener g_xdgToplevelListener = {
    XdgToplevelConfigure, XdgToplevelClose,
};

/* Wrap a gralloc handle in an android_wlegl_handle and turn it into a
 * wl_buffer — the client half of what server_wlegl reassembles. */
struct wl_buffer* CreateWleglBuffer(Globals& g, buffer_handle_t handle,
                                    int32_t width, int32_t height,
                                    int32_t stride, int32_t usage)
{
    const native_handle_t* nh = static_cast<const native_handle_t*>(handle);

    struct wl_array ints;
    wl_array_init(&ints);
    size_t intBytes = static_cast<size_t>(nh->numInts) * sizeof(int32_t);
    if (void* dst = wl_array_add(&ints, intBytes)) {
        memcpy(dst, &nh->data[nh->numFds], intBytes);
    }

    struct android_wlegl_handle* wh =
        android_wlegl_create_handle(g.wlegl, nh->numFds, &ints);
    wl_array_release(&ints);
    if (wh == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < nh->numFds; ++i) {
        android_wlegl_handle_add_fd(wh, nh->data[i]);
    }

    struct wl_buffer* buffer = android_wlegl_create_buffer(
        g.wlegl, width, height, stride, HAL_RGBA_8888, usage, wh);
    android_wlegl_handle_destroy(wh);
    return buffer;
}

/* A diagonal gradient that shifts each frame: any tearing, stale-buffer
 * reuse or wrong-stride bug is obvious on the panel. */
void PaintFrame(void* pixels, int32_t width, int32_t height,
                int32_t pixelStride, int frame)
{
    auto* row = static_cast<uint8_t*>(pixels);
    for (int32_t y = 0; y < height; ++y) {
        auto* px = reinterpret_cast<uint32_t*>(row);
        for (int32_t x = 0; x < width; ++x) {
            uint8_t r = static_cast<uint8_t>((x + frame * 4) & 0xFF);
            uint8_t gch = static_cast<uint8_t>((y + frame * 2) & 0xFF);
            uint8_t b = static_cast<uint8_t>(frame & 0xFF);
            px[x] = 0xFF000000u | (static_cast<uint32_t>(b) << 16) |
                    (static_cast<uint32_t>(gch) << 8) | r;
        }
        row += static_cast<size_t>(pixelStride) * 4;
    }
}

} // namespace

int main(int argc, char** argv)
{
    /* Unbuffered: this tool is usually killed by a timeout, and buffered
     * progress lines would be lost exactly when they matter. */
    setvbuf(stdout, nullptr, _IONBF, 0);

    int frames = (argc > 1) ? atoi(argv[1]) : 300;
    int32_t width  = (argc > 2) ? atoi(argv[2]) : 720;
    int32_t height = (argc > 3) ? atoi(argv[3]) : 1280;
    int32_t usage  = (argc > 4) ? static_cast<int32_t>(strtoul(argv[4], nullptr, 0))
                                : HARNESS_USAGE;

    struct wl_display* display = wl_display_connect(nullptr);
    if (display == nullptr) {
        fprintf(stderr, "connect failed (WAYLAND_DISPLAY / XDG_RUNTIME_DIR?)\n");
        return 1;
    }

    Globals g;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &g_registryListener, &g);
    wl_display_roundtrip(display);

    if (g.compositor == nullptr || g.wlegl == nullptr) {
        fprintf(stderr, "missing globals: compositor=%p android_wlegl=%p\n",
                static_cast<void*>(g.compositor), static_cast<void*>(g.wlegl));
        return 1;
    }
    printf("bound wl_compositor + android_wlegl%s\n",
           g.wmBase != nullptr ? " + xdg_wm_base" : " (no xdg_wm_base)");

    struct wl_surface* surface = wl_compositor_create_surface(g.compositor);
    if (g.wmBase != nullptr) {
        struct xdg_surface* xdgSurface =
            xdg_wm_base_get_xdg_surface(g.wmBase, surface);
        xdg_surface_add_listener(xdgSurface, &g_xdgSurfaceListener, nullptr);
        struct xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
        xdg_toplevel_add_listener(toplevel, &g_xdgToplevelListener, nullptr);
        xdg_toplevel_set_title(toplevel, "wlegl-harness");
        wl_surface_commit(surface);
        wl_display_roundtrip(display);
    }

    hybris_gralloc_initialize(0 /* no framebuffer */);

    /* Three buffers, like the container's hwcomposer.  Two is not
     * enough: RS holds both the buffer on screen and the one queued
     * behind it, so a two-buffer client deadlocks waiting for a release
     * that only a third frame can trigger. */
    constexpr int BUFFER_COUNT = 3;
    buffer_handle_t handles[BUFFER_COUNT] = {};
    struct wl_buffer* buffers[BUFFER_COUNT] = {};
    uint32_t grallocStride = 0;

    for (int i = 0; i < BUFFER_COUNT; ++i) {
        int rc = hybris_gralloc_allocate(width, height, HAL_RGBA_8888,
                                         usage, &handles[i],
                                         &grallocStride);
        if (rc != 0 || handles[i] == nullptr) {
            fprintf(stderr, "gralloc allocate %d failed: %d (usage 0x%x)\n",
                    i, rc, usage);
            return 1;
        }
        buffers[i] = CreateWleglBuffer(g, handles[i], width, height,
                                       static_cast<int32_t>(grallocStride),
                                       usage);
        if (buffers[i] == nullptr) {
            fprintf(stderr, "create_buffer %d failed\n", i);
            return 1;
        }
        wl_buffer_add_listener(buffers[i], &g_bufferListener, &g_busy[i]);
    }
    printf("allocated %d buffers %dx%d usage 0x%x, gralloc pixel stride %u\n",
           BUFFER_COUNT, width, height, usage, grallocStride);

    struct timespec start {};
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int frame = 0; frames == 0 || frame < frames; ++frame) {
        int idx = frame % BUFFER_COUNT;

        /* Wait until the server hands this buffer back.  Drawing into a
         * buffer the compositor still holds is a protocol violation and
         * would race RS's read of the very same dma-buf. */
        if (g_busy[idx]) {
            printf("frame %d: waiting for release of buffer %d\n", frame, idx);
        }
        while (g_busy[idx]) {
            if (wl_display_dispatch(display) < 0) {
                fprintf(stderr, "dispatch failed waiting for release\n");
                return 1;
            }
        }

        void* pixels = nullptr;
        if (hybris_gralloc_lock(handles[idx], usage, 0, 0,
                                width, height, &pixels) == 0 &&
            pixels != nullptr) {
            PaintFrame(pixels, width, height,
                       static_cast<int32_t>(grallocStride), frame);
            hybris_gralloc_unlock(handles[idx]);
        }

        g_busy[idx] = true;
        wl_surface_attach(surface, buffers[idx], 0, 0);
        wl_surface_damage(surface, 0, 0, width, height);
        wl_surface_commit(surface);
        if (wl_display_flush(display) < 0) {
            fprintf(stderr, "display error at frame %d\n", frame);
            return 1;
        }

        if (frame > 0 && frame % 60 == 0) {
            struct timespec now {};
            clock_gettime(CLOCK_MONOTONIC, &now);
            double secs = (now.tv_sec - start.tv_sec) +
                          (now.tv_nsec - start.tv_nsec) / 1e9;
            printf("frame %d: %.1f fps avg, %d releases\n",
                   frame, frame / secs, g_released);
            fflush(stdout);
        }
    }

    printf("done: %d frames, %d releases\n", frames, g_released);
    for (int i = 0; i < BUFFER_COUNT; ++i) {
        wl_buffer_destroy(buffers[i]);
        hybris_gralloc_release(handles[i], 1);
    }
    wl_display_disconnect(display);
    return 0;
}
