/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "wl_output_surface.h"

#include <hilog/log.h>
#include <sync_fence.h>
#include <transaction/rs_interfaces.h>
#include <transaction/rs_transaction.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "waydroid_output"

namespace OHOS {
namespace Waydroid {

namespace {
/* Above every normal window, like bootanimation's node. */
constexpr float SURFACE_NODE_Z = 100000.0f;

/* The producer queue must hold at least the container SurfaceFlinger's
 * buffer-cycle count (~3-4) plus slack for the on-screen buffer and one
 * in flight, or the queue pins full and every flush fails.  8 is roomy. */
constexpr uint32_t OUTPUT_QUEUE_SIZE = 8;
}

OutputSurface::~OutputSurface()
{
    Detach();
}

bool OutputSurface::AttachSelfDrawingNode(uint64_t screenId, int32_t width, int32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);

    Rosen::RSSurfaceNodeConfig config;
    config.SurfaceNodeName = "WaydroidOutputNode";
    config.isSync = false;
    auto node = Rosen::RSSurfaceNode::Create(
        config, Rosen::RSSurfaceNodeType::SELF_DRAWING_WINDOW_NODE, true, false);
    if (node == nullptr) {
        HILOG_ERROR(LOG_CORE, "AttachSelfDrawingNode: RSSurfaceNode::Create failed");
        return false;
    }

    node->SetPositionZ(SURFACE_NODE_Z);
    node->SetBounds({ 0, 0, width, height });
    node->SetBackgroundColor(0xFF000000);
    node->SetFrameGravity(Rosen::Gravity::RESIZE);
    Rosen::RSTransaction::FlushImplicitTransaction();

    /* Take the surface BEFORE attaching: GetSurface only converts the node's
     * own surface and has nothing to do with the display, so doing it first
     * means a failure here leaves an unattached node that simply dies with
     * the local shared_ptr — rather than a second fullscreen Z=100000 node
     * parented to the display with no owner to detach it. */
    sptr<Surface> surface = node->GetSurface();
    if (surface == nullptr) {
        HILOG_ERROR(LOG_CORE, "AttachSelfDrawingNode: node has no surface");
        return false;
    }
    surface->SetQueueSize(OUTPUT_QUEUE_SIZE);

    node_         = node;
    surface_      = surface;
    screenId_     = screenId;
    attachHealed_ = false;   /* fresh node: it gets its own single heal */
    InstallReleaseListenerLocked();
    width_   = width;
    height_  = height;

    AttachToDisplayLocked();
    Rosen::RSTransaction::FlushImplicitTransaction();
    HILOG_INFO(LOG_CORE, "output: self-drawing node on screen %{public}llu, %{public}dx%{public}d",
               static_cast<unsigned long long>(screenId), width, height);
    return true;
}

bool OutputSurface::AttachProducer(const sptr<IBufferProducer>& producer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (producer == nullptr) {
        return false;
    }
    sptr<IBufferProducer> p = producer;
    sptr<Surface> surface = Surface::CreateSurfaceAsProducer(p);
    if (surface == nullptr) {
        HILOG_ERROR(LOG_CORE, "AttachProducer: CreateSurfaceAsProducer failed");
        return false;
    }

    /* Releasing the self-drawing node here (rather than keeping it as a
     * fallback) is deliberate: two attached outputs would each get half
     * the commits. */
    if (node_ != nullptr) {
        /* screenId_, not 0: DetachToDisplay skips every logical display node
         * whose GetScreenId() != screenId, so a hardcoded 0 silently no-ops
         * on any other screen and strands our fullscreen Z=100000 node on
         * the tree, covering the very producer we are switching to. */
        node_->DetachToDisplay(screenId_);
        Rosen::RSTransaction::FlushImplicitTransaction();
        node_ = nullptr;
        screenId_ = 0;
        attachHealed_ = false;
    }
    surface->SetQueueSize(OUTPUT_QUEUE_SIZE);
    surface_ = surface;
    InstallReleaseListenerLocked();
    HILOG_INFO(LOG_CORE, "output: switched to app producer surface");
    return true;
}

void OutputSurface::Detach()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (node_ != nullptr) {
        node_->DetachToDisplay(screenId_);   /* see AttachProducer */
        Rosen::RSTransaction::FlushImplicitTransaction();
        node_ = nullptr;
        screenId_ = 0;
        attachHealed_ = false;
    }
    surface_ = nullptr;
}

void OutputSurface::AttachToDisplayLocked()
{
    if (node_ == nullptr) {
        return;   /* stage 2 (app producer): there is no node to attach. */
    }
    node_->AttachToDisplay(screenId_);
}

void OutputSurface::SetNodeVisible(bool visible)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (node_ != nullptr) {
        node_->SetVisible(visible);
        Rosen::RSTransaction::FlushImplicitTransaction();
    }
}

void OutputSurface::EnsureAttachedToDisplay()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (node_ == nullptr || attachHealed_) {
        return;   /* no node (stage 2), or the single heal is already spent */
    }
    attachHealed_ = true;
    AttachToDisplayLocked();
    Rosen::RSTransaction::FlushImplicitTransaction();
    HILOG_INFO(LOG_CORE, "output: re-issued AttachToDisplay for screen %{public}llu",
               static_cast<unsigned long long>(screenId_));
}

void OutputSurface::ResetQueue()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (surface_ == nullptr) {
        return;
    }
    /*
     * CleanCache(false), NOT (true).  Both empty the producer-side cache
     * (BufferQueue::ClearLocked, so the caller's retry attach is fresh, not
     * BUFFER_IS_INCACHE), but they differ in the consumer callback RS runs:
     *   - CleanCache(true)  -> RSRenderServiceListener::OnGoBackground(), which
     *     calls node->UpdateBufferInfo(nullptr, ...) — it NULLS the node's
     *     current buffer, blanking it until the next composite.  This pump
     *     fires several times a second on the BUFFER_IS_INCACHE path, so with
     *     (true) the panel flickered ~15x/s.
     *   - CleanCache(false) -> OnCleanCache(), which only resets the PRE-buffer
     *     and keeps the current displayed buffer — no blank frame.
     */
    GSError err = surface_->CleanCache(false);
    if (err != GSERROR_OK) {
        HILOG_WARN(LOG_CORE, "CleanCache failed: %{public}d", static_cast<int>(err));
    }
}

bool OutputSurface::IsAttached()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return surface_ != nullptr;
}

void OutputSurface::SetReleaseCallback(ReleaseCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    releaseCb_ = std::move(cb);
    InstallReleaseListenerLocked();
}

void OutputSurface::InstallReleaseListenerLocked()
{
    if (surface_ == nullptr || !releaseCb_) {
        return;
    }
    /*
     * RS releases the previous buffer every GPU-composited frame
     * (RSUniRenderThread::CollectReleaseTasks releases preBuffer whenever the
     * node is NOT hardware-composer-enabled, which is always the case for a
     * SELF_DRAWING_WINDOW_NODE — so this runs under plain GPU composition,
     * no hardware composer, no flicker).
     *
     * We must use the *backup* release listener.  The plain
     * RegisterReleaseListener(OnReleaseFunc) variant is delivered through
     * BufferReleaseProducerListener::OnBufferReleased(), which hard-codes a
     * NULL SurfaceBuffer, so the released buffer's identity is lost and it is
     * never recycled — that was the low-fps bug: the queue pinned full, every
     * AttachAndFlushBuffer returned QUEUE_FULL/BUFFER_IS_INCACHE, and only the
     * QUEUE_FULL mailbox fallback limped frames through, well below the
     * container's rate.  The backup variant (OnBufferReleasedWithFence)
     * delivers the real buffer, letting ReclaimReleased() recycle it.
     */
    GSError err = surface_->RegisterReleaseListenerBackup(
        [this](const sptr<SurfaceBuffer>& buffer, const sptr<SyncFence>& /*fence*/) -> GSError {
            if (buffer != nullptr) {
                ReclaimReleased(buffer);
            }
            return GSERROR_OK;
        });
    if (err != GSERROR_OK) {
        HILOG_WARN(LOG_CORE, "RegisterReleaseListenerBackup failed: %{public}d",
                   static_cast<int>(err));
    }
}

/*
 * Runs on the surface's release-listener (binder) thread when RS is done with
 * `released`.  A released buffer stays in the producer queue's cache, so the
 * container's next commit of it would return BUFFER_IS_INCACHE; RequestAnd-
 * DetachBuffer pops it back out.  Just as importantly, *issuing* this request
 * each release is what keeps RS's acquire/release cycle running: without a
 * producer-side request the queue stalls full and RS stops releasing (an A/B
 * on device: drop this call and releases go to zero and the panel throttles
 * to a fraction of the container's rate).  RequestAndDetachBuffer skips the
 * on-screen buffer, so the displayed frame is never pulled from under RS —
 * no tearing.  Any buffer it hands back is passed to the container hwc
 * (wl_buffer.release) so SurfaceFlinger can reuse it.
 */
void OutputSurface::ReclaimReleased(const sptr<SurfaceBuffer>& released)
{
    sptr<Surface> surf;
    ReleaseCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        surf = surface_;
        cb = releaseCb_;
    }
    if (surf == nullptr) {
        return;
    }
    BufferRequestConfig reqConfig = released->GetBufferRequestConfig();
    sptr<SurfaceBuffer> reclaimed;
    sptr<SyncFence> reclaimedFence;
    GSError err = surf->RequestAndDetachBuffer(reclaimed, reclaimedFence, reqConfig);
    if (err == GSERROR_OK && reclaimed != nullptr && cb) {
        cb(reclaimed.GetRefPtr());
    }
}

bool OutputSurface::Flush(const sptr<SurfaceBuffer>& buffer, int32_t acquireFence,
                          int32_t width, int32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (surface_ == nullptr || buffer == nullptr) {
        return false;
    }

    /* The wl_buffer's acquire fence (if the client sent one) becomes the
     * flush fence: RS waits on it instead of us blocking here. */
    sptr<SyncFence> fence = SyncFence::INVALID_FENCE;
    if (acquireFence >= 0) {
        fence = sptr<SyncFence>(new SyncFence(acquireFence));
    }

    BufferFlushConfig flushConfig = {
        .damage = { .x = 0, .y = 0, .w = width, .h = height },
        .timestamp = 0,
    };

    sptr<SurfaceBuffer> sb = buffer;

    /* First sighting goes through AttachAndFlushBuffer(needMap=false) — one
     * IPC and no gralloc CPU lock.  Buffers RS has since released are pulled
     * back out of the cache asynchronously by the release listener
     * (ReclaimReleased), so by the time the container re-commits one its slot
     * is free again. */
    GSError err = surface_->AttachAndFlushBuffer(sb, fence, flushConfig, false);

    /* Wake RenderService to composite THIS frame.  Our output is a
     * standalone self-drawing node with no ArkUI app requesting frames
     * for it, so RS would otherwise only composite it on its idle
     * heartbeat.  A forced next-vsync per flush makes RS acquire our buffer
     * (releasing the previous one) at the container's frame rate. */
    Rosen::RSInterfaces::GetInstance().ForceRefreshOneFrameWithNextVSync();

    return err == GSERROR_OK;
}

} // namespace Waydroid
} // namespace OHOS
