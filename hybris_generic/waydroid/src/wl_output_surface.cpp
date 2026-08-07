/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "wl_output_surface.h"

#include <hilog/log.h>
#include <sync_fence.h>
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
    node->AttachToDisplay(screenId);
    Rosen::RSTransaction::FlushImplicitTransaction();

    sptr<Surface> surface = node->GetSurface();
    if (surface == nullptr) {
        HILOG_ERROR(LOG_CORE, "AttachSelfDrawingNode: node has no surface");
        return false;
    }

    node_    = node;
    surface_ = surface;
    attached_.clear();
    reclaimable_.store(0);
    InstallReleaseListenerLocked();
    width_   = width;
    height_  = height;
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
        node_->DetachToDisplay(0);
        Rosen::RSTransaction::FlushImplicitTransaction();
        node_ = nullptr;
    }
    surface_ = surface;
    attached_.clear();
    reclaimable_.store(0);
    InstallReleaseListenerLocked();
    HILOG_INFO(LOG_CORE, "output: switched to app producer surface");
    return true;
}

void OutputSurface::Detach()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (node_ != nullptr) {
        node_->DetachToDisplay(0);
        Rosen::RSTransaction::FlushImplicitTransaction();
        node_ = nullptr;
    }
    surface_ = nullptr;
    attached_.clear();
}

void OutputSurface::ResetQueue()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (surface_ == nullptr) {
        return;
    }
    GSError err = surface_->CleanCache(true);
    if (err != GSERROR_OK) {
        HILOG_WARN(LOG_CORE, "CleanCache failed: %{public}d", static_cast<int>(err));
    }
    attached_.clear();
    reclaimable_.store(0);
    HILOG_INFO(LOG_CORE, "output: queue reset");
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

/* RS calls this on a binder thread when it is done with a buffer.  It is
 * the only correct trigger for wl_buffer.release: releasing earlier lets
 * the client redraw into a buffer RS is still reading (and, because the
 * buffer is then still in the queue cache, makes the next attach fail
 * with BUFFER_IS_INCACHE). */
void OutputSurface::InstallReleaseListenerLocked()
{
    if (surface_ == nullptr || !releaseCb_) {
        return;
    }
    ReleaseCallback cb = releaseCb_;
    GSError err = surface_->RegisterReleaseListener(
        [this, cb](sptr<SurfaceBuffer>& buffer) -> GSError {
            if (buffer != nullptr) {
                reclaimable_.fetch_add(1);
                cb(buffer.GetRefPtr());
            }
            return GSERROR_OK;
        });
    if (err != GSERROR_OK) {
        HILOG_WARN(LOG_CORE, "RegisterReleaseListener failed: %{public}d",
                   static_cast<int>(err));
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

    /*
     * Make room before every attach.
     *
     * Any buffer the queue has already seen is still in its cache (in
     * RELEASED state once RS is done): re-attaching it returns
     * BUFFER_IS_INCACHE, and FlushBuffer refuses anything but
     * REQUESTED/ATTACHED.  RequestAndDetachBuffer is the mirror of
     * AttachAndFlushBuffer (one IPC) and pops a free buffer out of the
     * cache entirely.  Whatever comes back is dropped — every buffer in
     * this queue is one of ours and the client owns its contents; the
     * point is only to free the slot.
     *
     * The request config must describe the buffer as it actually is,
     * strideAlignment included (the queue compares configs and would
     * otherwise reallocate a fresh buffer instead of handing ours back,
     * leaving the original stuck in the cache).
     */
    if (reclaimable_.load() > 0) {
        reclaimable_.fetch_sub(1);
        sptr<SurfaceBuffer> reclaimed;
        sptr<SyncFence> reclaimedFence;
        BufferRequestConfig reqConfig = {
            .width  = sb->GetWidth(),
            .height = sb->GetHeight(),
            .strideAlignment = sb->GetStride(),
            .format = sb->GetFormat(),
            .usage  = sb->GetUsage(),
            .timeout = 0,
            .colorGamut = sb->GetSurfaceBufferColorGamut(),
            .transform = sb->GetSurfaceBufferTransform(),
        };
        GSError rerr = surface_->RequestAndDetachBuffer(reclaimed, reclaimedFence,
                                                        reqConfig);
        if (rerr == GSERROR_OK && reclaimed != nullptr) {
            attached_.erase(reclaimed.GetRefPtr());
        } else if (rerr != GSERROR_NO_BUFFER) {
            HILOG_WARN(LOG_CORE, "reclaim failed: %{public}d", static_cast<int>(rerr));
        }
    }

    GSError err = surface_->AttachAndFlushBuffer(sb, fence, flushConfig, false);
    if (err != GSERROR_OK) {
        HILOG_ERROR(LOG_CORE, "attach+flush failed: %{public}d",
                    static_cast<int>(err));
        return false;
    }
    attached_.insert(sb.GetRefPtr());
    return true;
}

} // namespace Waydroid
} // namespace OHOS
