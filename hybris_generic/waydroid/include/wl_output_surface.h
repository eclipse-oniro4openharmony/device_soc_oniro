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

    /*
     * Re-issue AttachToDisplay for the self-drawing node.
     *
     * The attach we do at startup is normally DROPPED, and silently.
     * RSRenderNodeMap::AttachToDisplay only attaches to a logical display
     * node that already has IsOnTheTree() — and that node is published by
     * the display/window manager well after render_service starts answering
     * screen queries.  The supervisor starts us from init's boot /
     * post-fs-data stages, so at that point there is nothing to attach to
     * and the command is a no-op returning false.  Being up before
     * render_service makes it certain, but losing this race does NOT
     * require it: measured on ansuz with render_service already running
     * 3 s ahead of us, the attach still went nowhere.
     *
     * The failure is invisible from here — we keep a perfectly valid
     * Surface and go on flushing container frames into it forever, while RS
     * composites none of them.  Symptom: the front-end sits on its "Android
     * is starting…" placeholder (which lives BEHIND our Z=100000 node)
     * although the container booted fine.  Confirm with
     *   hidumper -s RenderService -a nodeNotOnTree | grep WaydroidOutputNode
     * Note `hidumper -s RenderService -a 'fps WaydroidOutputNode'` keeps
     * ticking either way — it counts producer flushes, not composition.
     *
     * RS's own retry (RSSurfaceRenderNode::AfterTreeStateChanged) cannot
     * save us: it is gated on attachedInfo_, which is only set once an
     * attach has ALREADY succeeded, so a first attach that was dropped is
     * never retried.  Hence this, called from ApplyVisibility before a show.
     *
     * Runs AT MOST ONCE per node, and that limit is load-bearing — a repeat
     * attach is NOT free.  RSRenderNodeMap::AttachToDisplay clears
     * attachedInfo_ as its first statement and only restores it when it
     * actually re-parents; if we are already the display's child it hits
     * `continue`, returns false, and the caller therefore leaves
     * attachedInfo_ empty.  So re-attaching an already-attached node
     * silently disarms the very RS recovery described above.  Once is
     * enough because the drop this heals is a startup-only condition.
     *
     * Residual gap: if the one heal also lands too early we do not try
     * again, and an RS restart (new node map, our id gone) is not covered
     * at all — that needs RSInterfaces::SetOnRemoteDiedCallback to rebuild
     * and re-attach the node, which is not wired up yet.
     */
    void EnsureAttachedToDisplay();

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
    /* Issue the attach command; caller holds mutex_ and flushes. */
    void AttachToDisplayLocked();
    /* Pull one RS-released buffer out of the queue cache (so the container
     * can re-attach it) and hand it back to the container.  Runs on the
     * surface's release-listener (binder) thread. */
    void ReclaimReleased(const sptr<SurfaceBuffer>& released);

    std::mutex mutex_;
    sptr<Surface> surface_;
    std::shared_ptr<Rosen::RSSurfaceNode> node_;   /* stage 1 only */
    ReleaseCallback releaseCb_;
    uint64_t screenId_ = 0;      /* screen node_ was attached to (stage 1) */
    bool attachHealed_ = false;  /* EnsureAttachedToDisplay spent on node_ */
    int32_t width_  = 0;
    int32_t height_ = 0;
    /* Request config of the buffers we flush; see ReclaimReleased. */
    BufferRequestConfig lastConfig_ {};
};

} // namespace Waydroid
} // namespace OHOS

#endif // WAYDROID_WL_OUTPUT_SURFACE_H
