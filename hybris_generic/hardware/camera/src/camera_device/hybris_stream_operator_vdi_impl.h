/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Hybris Stream Operator VDI — implements IStreamOperatorVdi by
 * bridging to libdroidmedia's Camera1 ABI (preview frames + JPEG
 * capture).  Driven by Droid::Loader from the device VDI.
 *
 * Lifecycle (per phase_n12_camera_droidmedia.md § N12.D.5–D.8):
 *
 *   CreateStreams                — stash VdiStreamInfo per id
 *   AttachBufferQueue            — wrap each producer in a Surface
 *   CommitStreams                — set Camera1 preview-size from the
 *                                  preview stream, install droidmedia
 *                                  callbacks
 *   Capture(streaming=true) on
 *     PREVIEW stream             — droid_media_camera_start_preview;
 *                                  frame callbacks start firing
 *   Capture(streaming=false) on
 *     STILL_CAPTURE stream       — droid_media_camera_take_picture;
 *                                  JPEG comes back via compressed_image_cb
 *   CancelCapture                — stop preview if it owned the captureId
 *
 * Threading: droidmedia callbacks (frame_available, compressed_image_cb)
 * run on a bionic-side worker thread (Halium ProcessState thread pool).
 * MVP calls OHOS Surface APIs and the OHOS IStreamOperatorVdiCallback
 * directly from those threads — works in practice because libhybris's
 * bionic ↔ musl shim covers the relevant pthread/std::mutex calls.  If
 * it ever proves unstable, the fix is the marshal pattern composer_host
 * already uses (SPSC queue draining on an OHOS-side worker).
 */

#ifndef HYBRIS_STREAM_OPERATOR_VDI_IMPL_H
#define HYBRIS_STREAM_OPERATOR_VDI_IMPL_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "droidmedia/droidmediacamera.h"
#include "surface.h"
#include "surface_type.h"
#include "v1_0/istream_operator_vdi.h"
#include "v1_0/istream_operator_vdi_callback.h"
#include "v1_0/vdi_types.h"

namespace OHOS::Camera::Hybris {

using OHOS::HDI::Camera::V1_0::BufferProducerSequenceable;
using OHOS::VDI::Camera::V1_0::IStreamOperatorVdi;
using OHOS::VDI::Camera::V1_0::IStreamOperatorVdiCallback;
using OHOS::VDI::Camera::V1_0::IOfflineStreamOperatorVdi;
using OHOS::VDI::Camera::V1_0::VdiCaptureInfo;
using OHOS::VDI::Camera::V1_0::VdiOperationMode;
using OHOS::VDI::Camera::V1_0::VdiStreamAttribute;
using OHOS::VDI::Camera::V1_0::VdiStreamInfo;
using OHOS::VDI::Camera::V1_0::VdiStreamSupportType;

class HybrisStreamOperatorVdiImpl : public IStreamOperatorVdi {
public:
    HybrisStreamOperatorVdiImpl(DroidMediaCamera *cam,
                                const std::string &ohosCameraId,
                                const sptr<IStreamOperatorVdiCallback> &cb);
    ~HybrisStreamOperatorVdiImpl() override;

    int32_t IsStreamsSupported(VdiOperationMode mode,
                               const std::vector<uint8_t> &modeSetting,
                               const std::vector<VdiStreamInfo> &infos,
                               VdiStreamSupportType &type) override;
    int32_t CreateStreams(const std::vector<VdiStreamInfo> &streamInfos) override;
    int32_t ReleaseStreams(const std::vector<int32_t> &streamIds) override;
    int32_t CommitStreams(VdiOperationMode mode,
                          const std::vector<uint8_t> &modeSetting) override;
    int32_t GetStreamAttributes(std::vector<VdiStreamAttribute> &attributes) override;
    int32_t AttachBufferQueue(int32_t streamId,
                              const sptr<BufferProducerSequenceable> &bufferProducer) override;
    int32_t DetachBufferQueue(int32_t streamId) override;
    int32_t Capture(int32_t captureId, const VdiCaptureInfo &info,
                    bool isStreaming) override;
    int32_t CancelCapture(int32_t captureId) override;
    int32_t ChangeToOfflineStream(const std::vector<int32_t> &streamIds,
                                  const sptr<IStreamOperatorVdiCallback> &callbackObj,
                                  sptr<IOfflineStreamOperatorVdi> &offlineOperator) override;

private:
    struct StreamRecord {
        VdiStreamInfo                       info;
        sptr<BufferProducerSequenceable>    producer;
        sptr<OHOS::Surface>                 surface;   /* wraps producer_ */
        BufferRequestConfig                 requestConfig{};
    };

    /* Resolve the first preview-intent stream id from streams_.  -1 if
     * none.  Used to pick which stream backs StartPreview frames. */
    int32_t FindPreviewStreamId() const;
    int32_t FindStillCaptureStreamId() const;

    /* Install droidmedia callbacks once we have a buffer queue + cam.
     * Idempotent. */
    void InstallDroidCallbacks();

    /* Stop preview if running; releases cached bq_ callbacks. */
    void StopPreviewLocked();

    /* ─── droidmedia bionic-side callbacks ──────────────────────────
     * Static thunks dispatch to the member functions via the `data`
     * void pointer registered with set_callbacks. */
    static bool OnFrameAvailableThunk(void *data, DroidMediaBuffer *buf);
    static bool OnBufferCreatedThunk(void *data, DroidMediaBuffer *buf);
    static void OnBuffersReleasedThunk(void *data);
    static void OnCompressedImageThunk(void *data, DroidMediaData *mem);
    static void OnShutterThunk(void *data);

    /* Per-call handlers run on the droidmedia worker thread. */
    bool OnFrameAvailable(DroidMediaBuffer *buf);
    void OnCompressedImage(const DroidMediaData *mem);
    void OnShutter();

    /* Copy YUV planes from a droidmedia DroidMediaBufferYCbCr into a
     * RequestBuffer'd OHOS SurfaceBuffer.  Handles NV21 (yuv.cr < yuv.cb)
     * directly via memcpy + handles NV12 by swapping U/V during copy.
     * Returns true on success.  Caller still owns both buffers. */
    bool CopyYuvToSurfaceBuffer(const DroidMediaBufferInfo &info,
                                 const DroidMediaBufferYCbCr &yuv,
                                 OHOS::SurfaceBuffer *out);

    std::mutex                              mutex_;
    DroidMediaCamera                       *cam_ = nullptr;     /* borrowed */
    std::string                             ohosCameraId_;
    sptr<IStreamOperatorVdiCallback>        streamCallback_;

    std::map<int32_t, StreamRecord>         streams_;
    bool                                    configured_ = false;

    /* Preview-side state */
    DroidMediaBufferQueue                  *bq_              = nullptr;   /* borrowed */
    bool                                    previewStarted_  = false;
    bool                                    callbacksInstalled_ = false;
    int32_t                                 activePreviewStream_  = -1;
    int32_t                                 activePreviewCapture_ = -1;
    DroidMediaBufferQueueCallbacks          bqCallbacks_{};

    /* JPEG-side state.  We only ever have one capture in flight; the
     * framework serialises this for us. */
    int32_t                                 jpegStreamId_     = -1;
    int32_t                                 jpegCaptureId_    = -1;
    DroidMediaCameraCallbacks               cameraCallbacks_{};
};

} // namespace OHOS::Camera::Hybris

#endif // HYBRIS_STREAM_OPERATOR_VDI_IMPL_H
