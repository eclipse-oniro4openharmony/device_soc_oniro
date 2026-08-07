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
}

bool OutputSurface::IsAttached()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return surface_ != nullptr;
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
    GSError err = surface_->AttachAndFlushBuffer(sb, fence, flushConfig, false);
    if (err != GSERROR_OK) {
        HILOG_ERROR(LOG_CORE, "AttachAndFlushBuffer failed: %{public}d", static_cast<int>(err));
        return false;
    }
    return true;
}

} // namespace Waydroid
} // namespace OHOS
