/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * OutputSurface — where the container's frames land.
 *
 * Two interchangeable sources of the producer Surface (plan D4):
 *   stage 1  self-drawing RSSurfaceNode created by this service
 *            (bootanimation pattern — fullscreen, needs no app);
 *   stage 2  an ArkUI XComponent's producer handed over binder.
 * Everything below the Surface abstraction is identical, so the output
 * can be swapped at runtime without touching the frame path.
 */

#ifndef WAYDROID_WL_OUTPUT_SURFACE_H
#define WAYDROID_WL_OUTPUT_SURFACE_H

#include <cstdint>
#include <functional>
#include <mutex>

#include <surface.h>
#include <surface_buffer.h>
#include <ui/rs_surface_node.h>

namespace OHOS {
namespace Waydroid {

class OutputSurface {
public:
    OutputSurface() = default;
    ~OutputSurface();

    OutputSurface(const OutputSurface&) = delete;
    OutputSurface& operator=(const OutputSurface&) = delete;

    /*
     * Stage 1: create a fullscreen self-drawing RSSurfaceNode on
     * `screenId` and take its Surface.  Allowed for native SA tokens
     * (rs_client_to_render_connection_stub gates SELF_DRAWING_WINDOW_NODE
     * on exactly that).
     */
    bool AttachSelfDrawingNode(uint64_t screenId, int32_t width, int32_t height);

    /*
     * Stage 2: adopt a producer received over binder from the front-end
     * app (OH_NativeWindow_ReadFromParcel / IBufferProducer). Replaces
     * any current output; the caller keeps the death recipient.
     */
    bool AttachProducer(const sptr<IBufferProducer>& producer);

    /* Drop the current output; frames are discarded until re-attached. */
    void Detach();

    /* Toggle the self-drawing node's visibility WITHOUT destroying it, so
     * its last frame is retained across a W5 hide/show — otherwise a fresh
     * node shows black until the just-thawed container happens to redraw. */
    void SetNodeVisible(bool visible);

    /* Drop every buffer the queue is holding for us — used when a client
     * disconnects, since its buffers occupy queue slots that nothing
     * will ever release. */
    void ResetQueue();

    bool IsAttached();

    /*
     * Queue one already-imported buffer.
     *
     * First sighting of a buffer goes through
     * AttachAndFlushBuffer(needMap=false) — one IPC and no gralloc CPU
     * lock (the plain AttachBufferToQueue path hardcodes needMap=true
     * and would drag render_service through a CPU map per attach).
     * After that the buffer lives in the queue's cache and re-attaching
     * it returns GSERROR_BUFFER_IS_INCACHE, so later commits are a
     * plain FlushBuffer.
     */
    bool Flush(const sptr<SurfaceBuffer>& buffer, int32_t acquireFence,
               int32_t width, int32_t height);

    /*
     * Called from a binder thread whenever RS is done with a buffer.
     * The callback must be cheap and thread-safe: the server turns it
     * into a wl_buffer.release on its own thread.
     */
    using ReleaseCallback = std::function<void(SurfaceBuffer*)>;
    void SetReleaseCallback(ReleaseCallback cb);

    int32_t Width()  { return width_; }
    int32_t Height() { return height_; }

private:
    void InstallReleaseListenerLocked();
    /* Pull one RS-released buffer out of the queue cache (so the container
     * can re-attach it) and hand it back to the container.  Runs on the
     * surface's release-listener (binder) thread. */
    void ReclaimReleased(const sptr<SurfaceBuffer>& released);

    std::mutex mutex_;
    sptr<Surface> surface_;
    std::shared_ptr<Rosen::RSSurfaceNode> node_;   /* stage 1 only */
    ReleaseCallback releaseCb_;
    int32_t width_  = 0;
    int32_t height_ = 0;
};

} // namespace Waydroid
} // namespace OHOS

#endif // WAYDROID_WL_OUTPUT_SURFACE_H
