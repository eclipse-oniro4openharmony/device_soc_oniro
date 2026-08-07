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

    bool IsAttached();

    /*
     * Queue one already-imported buffer.  Uses
     * AttachAndFlushBuffer(needMap=false): a single IPC, no gralloc CPU
     * lock — the plain AttachBufferToQueue path hardcodes needMap=true
     * and would drag render_service through a CPU map per attach.
     */
    bool Flush(const sptr<SurfaceBuffer>& buffer, int32_t acquireFence,
               int32_t width, int32_t height);

    int32_t Width()  { return width_; }
    int32_t Height() { return height_; }

private:
    std::mutex mutex_;
    sptr<Surface> surface_;
    std::shared_ptr<Rosen::RSSurfaceNode> node_;   /* stage 1 only */
    int32_t width_  = 0;
    int32_t height_ = 0;
};

} // namespace Waydroid
} // namespace OHOS

#endif // WAYDROID_WL_OUTPUT_SURFACE_H
