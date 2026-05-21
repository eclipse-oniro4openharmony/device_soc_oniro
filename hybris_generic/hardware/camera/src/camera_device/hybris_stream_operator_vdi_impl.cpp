/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hybris_stream_operator_vdi_impl.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <unistd.h>

#include "droidmedia/droidmedia_loader.h"
#include "hybris_camera_log.h"
#include "ibuffer_producer.h"
#include "surface_buffer.h"

namespace OHOS::Camera::Hybris {

namespace V = OHOS::VDI::Camera::V1_0;

namespace {

/* Camera1 CAMERA_MSG_* values reused by libdroidmedia's take_picture
 * msgType parameter — see Camera.h in AOSP frameworks/base.  We only
 * need SHUTTER + COMPRESSED_IMAGE for stills. */
constexpr int kCameraMsgShutter         = 0x0002;
constexpr int kCameraMsgCompressedImage = 0x0100;

/* OHOS graphic PixelFormat values we receive in VdiStreamInfo.format_.
 * NV21 (25) is the format hybris_camera_ability.cpp advertises for
 * preview; BLOB (38) is the still-capture container. */
constexpr int32_t kGraphicPixelFmtBLOB = 38;

/* GRALLOC_USAGE_* — OHOS uses Android-compatible numeric values. */
constexpr uint64_t kGrallocUsageSwReadOften    = 0x3ULL;
constexpr uint64_t kGrallocUsageHwTexture      = 0x100ULL;
constexpr uint64_t kGrallocUsageHwCameraWrite  = 0x20000ULL;

constexpr int32_t kStrideAlignment = 8;

uint64_t UsageForStream(int32_t graphicFormat, V::VdiStreamIntent intent)
{
    if (graphicFormat == kGraphicPixelFmtBLOB ||
        intent == V::STILL_CAPTURE) {
        return kGrallocUsageSwReadOften | kGrallocUsageHwCameraWrite;
    }
    return kGrallocUsageSwReadOften | kGrallocUsageHwTexture |
           kGrallocUsageHwCameraWrite;
}

} // namespace

HybrisStreamOperatorVdiImpl::HybrisStreamOperatorVdiImpl(
    DroidMediaCamera *cam, const std::string &ohosCameraId,
    const sptr<IStreamOperatorVdiCallback> &cb)
    : cam_(cam), ohosCameraId_(ohosCameraId), streamCallback_(cb)
{
    CAMERA_VDI_LOGI("HybrisStreamOperatorVdiImpl ctor for %{public}s cam=%{public}p",
                    ohosCameraId_.c_str(), (void *)cam_);
}

HybrisStreamOperatorVdiImpl::~HybrisStreamOperatorVdiImpl()
{
    CAMERA_VDI_LOGI("HybrisStreamOperatorVdiImpl dtor for %{public}s",
                    ohosCameraId_.c_str());
    /*
     * Detach droidmedia callbacks before we vanish so a late
     * frame_available / compressed_image_cb doesn't land on a freed
     * `this`.  Caveat: upstream droidmedia's set_callbacks /
     * buffer_queue_set_callbacks dereference `*cb` unconditionally —
     * passing nullptr would SIGSEGV inside libdroidmedia (observed
     * 2026-05-21 during teardown).  Install empty (zeroed) callback
     * structs instead so the field copy succeeds and every function
     * pointer is NULL afterwards.
     */
    std::lock_guard<std::mutex> lock(mutex_);
    StopPreviewLocked();
    if (callbacksInstalled_) {
        if (bq_ != nullptr) {
            DroidMediaBufferQueueCallbacks empty{};
            Droid::Loader::Get().BufferQueueSetCallbacks(bq_, &empty, nullptr);
        }
        if (cam_ != nullptr) {
            DroidMediaCameraCallbacks empty{};
            Droid::Loader::Get().SetCallbacks(cam_, &empty, nullptr);
        }
        callbacksInstalled_ = false;
    }
}

int32_t HybrisStreamOperatorVdiImpl::IsStreamsSupported(
    VdiOperationMode mode, const std::vector<uint8_t> &modeSetting,
    const std::vector<VdiStreamInfo> &infos, VdiStreamSupportType &type)
{
    (void)mode;
    (void)modeSetting;
    (void)infos;
    type = V::DYNAMIC_SUPPORTED;
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::CreateStreams(
    const std::vector<VdiStreamInfo> &streamInfos)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream summary;
    for (size_t i = 0; i < streamInfos.size(); ++i) {
        const auto &s = streamInfos[i];
        if (i) summary << "; ";
        summary << "id=" << s.streamId_
                << " " << s.width_ << "x" << s.height_
                << " fmt=" << s.format_
                << " intent=" << static_cast<int32_t>(s.intent_)
                << " tunneled=" << (s.tunneledMode_ ? "1" : "0");

        StreamRecord rec;
        rec.info = s;
        rec.requestConfig.width            = s.width_;
        rec.requestConfig.height           = s.height_;
        rec.requestConfig.strideAlignment  = kStrideAlignment;
        rec.requestConfig.format           = s.format_;
        rec.requestConfig.usage            = UsageForStream(s.format_, s.intent_);
        rec.requestConfig.timeout          = 0;
        streams_[s.streamId_] = std::move(rec);
    }
    CAMERA_VDI_LOGI("CreateStreams(%{public}s): %{public}zu streams [%{public}s]",
                    ohosCameraId_.c_str(), streamInfos.size(),
                    summary.str().c_str());
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::ReleaseStreams(
    const std::vector<int32_t> &streamIds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    bool affectedPreview = false;
    bool affectedJpeg    = false;
    for (int32_t id : streamIds) {
        if (id == activePreviewStream_) affectedPreview = true;
        if (id == jpegStreamId_)        affectedJpeg    = true;
        streams_.erase(id);
    }
    if (affectedPreview) StopPreviewLocked();
    if (affectedJpeg) {
        jpegStreamId_  = -1;
        jpegCaptureId_ = -1;
    }
    CAMERA_VDI_LOGI("ReleaseStreams(%{public}s): %{public}zu streams "
                    "(stoppedPreview=%{public}d droppedJpeg=%{public}d)",
                    ohosCameraId_.c_str(), streamIds.size(),
                    (int)affectedPreview, (int)affectedJpeg);
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::CommitStreams(
    VdiOperationMode mode, const std::vector<uint8_t> &modeSetting)
{
    (void)modeSetting;
    std::lock_guard<std::mutex> lock(mutex_);

    if (cam_ == nullptr) {
        return V::CAMERA_CLOSED;
    }
    if (streams_.empty()) {
        CAMERA_VDI_LOGE("CommitStreams(%{public}s): no streams to commit",
                        ohosCameraId_.c_str());
        return V::INVALID_ARGUMENT;
    }

    /* Pick the preview stream's resolution to drive Camera1
     * preview-size; if none, pick the first stream as a fallback. */
    int32_t pvw = -1, pvh = -1;
    int32_t pvId = FindPreviewStreamId();
    if (pvId >= 0) {
        const auto &p = streams_[pvId].info;
        pvw = p.width_;
        pvh = p.height_;
    } else {
        const auto &first = streams_.begin()->second.info;
        pvw = first.width_;
        pvh = first.height_;
        CAMERA_VDI_LOGW("CommitStreams(%{public}s): no PREVIEW intent; using "
                        "streamId=%{public}d (%{public}dx%{public}d) for preview-size",
                        ohosCameraId_.c_str(), first.streamId_, pvw, pvh);
    }

    /*
     * Camera1 setParameters with the full key=value;key=value string
     * round-trips most HAL state — to change a single key the
     * canonical AOSP pattern is `params = camera->getParameters();
     * params.set(...); camera->setParameters(params)`.  We can't
     * round-trip through `Parameters` here (it's a C++-only class
     * inside libcamera_client) so we approximate by fetching the
     * current full string, mutating the preview-size= field in place,
     * and feeding the whole string back.  Setting only "preview-size="
     * alone makes MTK's HAL reject the call entirely (observed
     * 2026-05-21: set_parameters("preview-size=1280x720;preview-format
     * =yuv420sp") returned false even though both values are in the
     * supported lists).
     */
    auto &loader = Droid::Loader::Get();
    std::string params;
    if (char *cur = loader.GetParameters(cam_)) {
        params.assign(cur);
        std::free(cur);
    }
    if (params.empty()) {
        CAMERA_VDI_LOGW("CommitStreams(%{public}s): get_parameters returned empty "
                        "— skipping preview-size mutation",
                        ohosCameraId_.c_str());
    } else {
        char target[32];
        std::snprintf(target, sizeof(target), "preview-size=%dx%d", pvw, pvh);
        size_t key = params.find("preview-size=");
        if (key != std::string::npos) {
            size_t end = params.find(';', key);
            if (end == std::string::npos) end = params.size();
            params.replace(key, end - key, target);
        } else {
            /* Key absent — shouldn't happen for Camera1 HALs but be
             * defensive: append. */
            if (!params.empty() && params.back() != ';') params.push_back(';');
            params.append(target);
        }
        if (!loader.SetParameters(cam_, params.c_str())) {
            CAMERA_VDI_LOGW("CommitStreams(%{public}s): set_parameters "
                            "(round-tripped %{public}zu B with preview-size=%{public}dx%{public}d) "
                            "returned false — HAL may need finer tweaking",
                            ohosCameraId_.c_str(), params.size(), pvw, pvh);
        } else {
            CAMERA_VDI_LOGI("CommitStreams(%{public}s): set_parameters OK "
                            "preview-size=%{public}dx%{public}d",
                            ohosCameraId_.c_str(), pvw, pvh);
        }
    }

    /* Resolve a JPEG stream now if present so SHUTTER → take_picture
     * dispatch knows where to flush. */
    int32_t jpgId = FindStillCaptureStreamId();
    if (jpgId >= 0) {
        jpegStreamId_ = jpgId;
        CAMERA_VDI_LOGI("CommitStreams(%{public}s): jpeg stream id=%{public}d",
                        ohosCameraId_.c_str(), jpegStreamId_);
    }

    InstallDroidCallbacks();

    configured_ = true;
    CAMERA_VDI_LOGI("CommitStreams(%{public}s): mode=%{public}d "
                    "configured %{public}zu streams via droidmedia",
                    ohosCameraId_.c_str(), static_cast<int32_t>(mode),
                    streams_.size());
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::GetStreamAttributes(
    std::vector<VdiStreamAttribute> &attributes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    attributes.clear();
    attributes.reserve(streams_.size());
    for (const auto &kv : streams_) {
        VdiStreamAttribute attr = {};
        attr.streamId_              = kv.second.info.streamId_;
        attr.width_                 = kv.second.info.width_;
        attr.height_                = kv.second.info.height_;
        attr.overrideFormat_        = kv.second.info.format_;
        attr.overrideDataspace_     = kv.second.info.dataspace_;
        attr.producerUsage_         = kv.second.requestConfig.usage;
        attr.producerBufferCount_   = 4;
        attr.maxBatchCaptureCount_  = 1;
        attr.maxCaptureCount_       = 1;
        attributes.push_back(attr);
    }
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::AttachBufferQueue(
    int32_t streamId,
    const sptr<BufferProducerSequenceable> &bufferProducer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        CAMERA_VDI_LOGE("AttachBufferQueue(%{public}s): unknown streamId=%{public}d",
                        ohosCameraId_.c_str(), streamId);
        return V::INVALID_ARGUMENT;
    }
    if (bufferProducer == nullptr || bufferProducer->producer_ == nullptr) {
        CAMERA_VDI_LOGE("AttachBufferQueue(%{public}s): null producer for streamId=%{public}d",
                        ohosCameraId_.c_str(), streamId);
        return V::INVALID_ARGUMENT;
    }
    it->second.producer = bufferProducer;
    /* Wrap the IBufferProducer in a producer-side Surface so we can call
     * RequestBuffer / FlushBuffer / CancelBuffer.  Cheap to keep around
     * for the lifetime of the stream. */
    sptr<OHOS::IBufferProducer> rawProducer = bufferProducer->producer_;
    it->second.surface = OHOS::Surface::CreateSurfaceAsProducer(rawProducer);
    if (it->second.surface == nullptr) {
        CAMERA_VDI_LOGE("AttachBufferQueue(%{public}s): CreateSurfaceAsProducer "
                        "FAILED for streamId=%{public}d", ohosCameraId_.c_str(),
                        streamId);
        return V::DEVICE_ERROR;
    }
    CAMERA_VDI_LOGI("AttachBufferQueue(%{public}s): streamId=%{public}d "
                    "producer=%{public}p surface=%{public}p",
                    ohosCameraId_.c_str(), streamId,
                    bufferProducer.GetRefPtr(),
                    it->second.surface.GetRefPtr());
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::DetachBufferQueue(int32_t streamId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(streamId);
    if (it != streams_.end()) {
        it->second.producer = nullptr;
        it->second.surface  = nullptr;
    }
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::Capture(int32_t captureId,
                                              const VdiCaptureInfo &info,
                                              bool isStreaming)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (cam_ == nullptr || !configured_) {
        return V::CAMERA_CLOSED;
    }
    if (info.streamIds_.empty()) {
        return V::INVALID_ARGUMENT;
    }

    /*
     * Route by intent of the first stream in the request.  OHOS camera
     * framework issues separate Capture calls for preview vs JPEG so
     * each request maps cleanly to one droidmedia action.
     */
    int32_t firstId = info.streamIds_[0];
    auto it = streams_.find(firstId);
    if (it == streams_.end()) {
        CAMERA_VDI_LOGE("Capture(%{public}s): captureId=%{public}d unknown streamId=%{public}d",
                        ohosCameraId_.c_str(), captureId, firstId);
        return V::INVALID_ARGUMENT;
    }
    const auto intent = it->second.info.intent_;

    if (intent == V::STILL_CAPTURE) {
        if (!previewStarted_) {
            /* Camera1 requires preview running before take_picture. */
            CAMERA_VDI_LOGW("Capture(%{public}s): STILL_CAPTURE captureId=%{public}d "
                            "without active preview — Camera1 take_picture will fail",
                            ohosCameraId_.c_str(), captureId);
        }
        jpegStreamId_  = firstId;
        jpegCaptureId_ = captureId;
        CAMERA_VDI_LOGI("Capture(%{public}s): STILL_CAPTURE captureId=%{public}d "
                        "streamId=%{public}d → take_picture",
                        ohosCameraId_.c_str(), captureId, firstId);
        if (!Droid::Loader::Get().TakePicture(cam_,
                kCameraMsgShutter | kCameraMsgCompressedImage)) {
            CAMERA_VDI_LOGE("Capture(%{public}s): take_picture FAILED",
                            ohosCameraId_.c_str());
            jpegCaptureId_ = -1;
            return V::DEVICE_ERROR;
        }
        if (streamCallback_ != nullptr) {
            streamCallback_->OnCaptureStarted(captureId, info.streamIds_);
        }
        return V::NO_ERROR;
    }

    /* PREVIEW / VIDEO / other — kick the preview loop if not running.
     * Streaming flag is informational; droidmedia preview runs until
     * explicit stop. */
    activePreviewStream_  = firstId;
    activePreviewCapture_ = captureId;
    if (!previewStarted_) {
        if (!Droid::Loader::Get().StartPreview(cam_)) {
            CAMERA_VDI_LOGE("Capture(%{public}s): start_preview FAILED",
                            ohosCameraId_.c_str());
            activePreviewCapture_ = -1;
            return V::DEVICE_ERROR;
        }
        previewStarted_ = true;
        CAMERA_VDI_LOGI("Capture(%{public}s): start_preview OK "
                        "(captureId=%{public}d streamId=%{public}d streaming=%{public}d)",
                        ohosCameraId_.c_str(), captureId, firstId, (int)isStreaming);
    } else {
        CAMERA_VDI_LOGI("Capture(%{public}s): preview already running, "
                        "rerouting captureId=%{public}d → streamId=%{public}d",
                        ohosCameraId_.c_str(), captureId, firstId);
    }
    if (streamCallback_ != nullptr) {
        streamCallback_->OnCaptureStarted(captureId, info.streamIds_);
    }
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::CancelCapture(int32_t captureId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    CAMERA_VDI_LOGI("CancelCapture(%{public}s): captureId=%{public}d",
                    ohosCameraId_.c_str(), captureId);
    if (captureId == activePreviewCapture_) {
        StopPreviewLocked();
    }
    if (captureId == jpegCaptureId_) {
        jpegCaptureId_ = -1;
    }
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::ChangeToOfflineStream(
    const std::vector<int32_t> &streamIds,
    const sptr<IStreamOperatorVdiCallback> &callbackObj,
    sptr<IOfflineStreamOperatorVdi> &offlineOperator)
{
    (void)streamIds;
    (void)callbackObj;
    offlineOperator = nullptr;
    return V::METHOD_NOT_SUPPORTED;
}

/* ─── Internal helpers ─────────────────────────────────────────────── */

int32_t HybrisStreamOperatorVdiImpl::FindPreviewStreamId() const
{
    for (const auto &kv : streams_) {
        if (kv.second.info.intent_ == V::PREVIEW) {
            return kv.first;
        }
    }
    return -1;
}

int32_t HybrisStreamOperatorVdiImpl::FindStillCaptureStreamId() const
{
    for (const auto &kv : streams_) {
        if (kv.second.info.intent_ == V::STILL_CAPTURE) {
            return kv.first;
        }
    }
    return -1;
}

void HybrisStreamOperatorVdiImpl::InstallDroidCallbacks()
{
    if (callbacksInstalled_ || cam_ == nullptr) {
        return;
    }
    auto &loader = Droid::Loader::Get();
    bq_ = loader.GetBufferQueue(cam_);
    if (bq_ == nullptr) {
        CAMERA_VDI_LOGW("InstallDroidCallbacks(%{public}s): get_buffer_queue "
                        "returned NULL — preview frames will not be delivered",
                        ohosCameraId_.c_str());
        /* Compressed image / JPEG path can still work without the bq. */
    } else {
        bqCallbacks_ = {};
        bqCallbacks_.frame_available   = &HybrisStreamOperatorVdiImpl::OnFrameAvailableThunk;
        bqCallbacks_.buffer_created    = &HybrisStreamOperatorVdiImpl::OnBufferCreatedThunk;
        bqCallbacks_.buffers_released  = &HybrisStreamOperatorVdiImpl::OnBuffersReleasedThunk;
        loader.BufferQueueSetCallbacks(bq_, &bqCallbacks_, this);
    }
    cameraCallbacks_ = {};
    cameraCallbacks_.compressed_image_cb = &HybrisStreamOperatorVdiImpl::OnCompressedImageThunk;
    cameraCallbacks_.shutter_cb          = &HybrisStreamOperatorVdiImpl::OnShutterThunk;
    loader.SetCallbacks(cam_, &cameraCallbacks_, this);

    callbacksInstalled_ = true;
    CAMERA_VDI_LOGI("InstallDroidCallbacks(%{public}s): bq=%{public}p installed "
                    "(frame_available + compressed_image_cb)",
                    ohosCameraId_.c_str(), (void *)bq_);
}

void HybrisStreamOperatorVdiImpl::StopPreviewLocked()
{
    if (!previewStarted_) {
        return;
    }
    if (cam_ != nullptr) {
        Droid::Loader::Get().StopPreview(cam_);
    }
    previewStarted_       = false;
    activePreviewStream_  = -1;
    activePreviewCapture_ = -1;
    CAMERA_VDI_LOGI("StopPreviewLocked(%{public}s)", ohosCameraId_.c_str());
}

/* ─── droidmedia callback thunks ───────────────────────────────────── */

bool HybrisStreamOperatorVdiImpl::OnFrameAvailableThunk(void *data,
                                                         DroidMediaBuffer *buf)
{
    auto *self = static_cast<HybrisStreamOperatorVdiImpl *>(data);
    return self ? self->OnFrameAvailable(buf) : false;
}

bool HybrisStreamOperatorVdiImpl::OnBufferCreatedThunk(void *data,
                                                        DroidMediaBuffer *buf)
{
    (void)data;
    auto &loader = Droid::Loader::Get();
    DroidMediaBufferInfo info{};
    if (buf != nullptr) {
        loader.BufferGetInfo(buf, &info);
        CAMERA_VDI_LOGI("OnBufferCreated: %{public}ux%{public}u fmt=%{public}d "
                        "stride=%{public}d",
                        info.width, info.height, info.format, info.stride);
    }
    return true;
}

void HybrisStreamOperatorVdiImpl::OnBuffersReleasedThunk(void *data)
{
    (void)data;
    CAMERA_VDI_LOGI("OnBuffersReleased");
}

void HybrisStreamOperatorVdiImpl::OnCompressedImageThunk(void *data,
                                                          DroidMediaData *mem)
{
    auto *self = static_cast<HybrisStreamOperatorVdiImpl *>(data);
    if (self != nullptr && mem != nullptr) {
        self->OnCompressedImage(mem);
    }
}

void HybrisStreamOperatorVdiImpl::OnShutterThunk(void *data)
{
    auto *self = static_cast<HybrisStreamOperatorVdiImpl *>(data);
    if (self != nullptr) {
        self->OnShutter();
    }
}

/* ─── droidmedia callbacks (bionic-side worker thread) ─────────────── */

bool HybrisStreamOperatorVdiImpl::OnFrameAvailable(DroidMediaBuffer *buf)
{
    if (buf == nullptr) return false;

    auto &loader = Droid::Loader::Get();
    DroidMediaBufferInfo info{};
    loader.BufferGetInfo(buf, &info);
    /*
     * Log the first frame + every 30th frame to make preview activity
     * visible in hilog without flooding the log at 30 fps.  Static
     * counter is OK — droidmedia uses a single worker thread, so the
     * ++ doesn't race even though we read it before taking mutex_. */
    static std::atomic<uint32_t> kFrameCount{0};
    uint32_t n = kFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n % 30) == 0) {
        CAMERA_VDI_LOGI("OnFrameAvailable: frame #%{public}u %{public}ux%{public}u "
                        "fmt=%{public}d stride=%{public}d ts=%{public}lld",
                        n, info.width, info.height, info.format, info.stride,
                        (long long)info.timestamp);
    }

    DroidMediaBufferYCbCr yuv{};
    if (!loader.BufferLockYCbCr(buf, DROID_MEDIA_BUFFER_LOCK_READ, &yuv)) {
        loader.BufferRelease(buf, nullptr, nullptr);
        return false;
    }

    /* Snapshot the producer + captureId under the lock so we don't
     * trip on a concurrent CancelCapture / DetachBufferQueue. */
    sptr<OHOS::Surface>             surface;
    sptr<IStreamOperatorVdiCallback> cb;
    int32_t streamId  = -1;
    int32_t captureId = -1;
    BufferRequestConfig             config{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        streamId  = activePreviewStream_;
        captureId = activePreviewCapture_;
        cb        = streamCallback_;
        if (streamId >= 0) {
            auto it = streams_.find(streamId);
            if (it != streams_.end()) {
                surface = it->second.surface;
                config  = it->second.requestConfig;
            }
        }
    }

    if (surface == nullptr) {
        loader.BufferUnlock(buf);
        loader.BufferRelease(buf, nullptr, nullptr);
        return true;
    }

    /* Match the BufferRequestConfig to what the frame actually carries.
     * If the HAL emitted a different size than we asked for, requesting
     * a buffer at the registered config and copying into it is still
     * correct as long as the dst is at least as big as the src — Halium
     * occasionally rounds up to a hardware-friendly stride. */
    sptr<OHOS::SurfaceBuffer> sb;
    int32_t fence = -1;
    auto rc = surface->RequestBuffer(sb, fence, config);
    if (fence >= 0) {
        ::close(fence);
    }
    if (rc != OHOS::GSERROR_OK || sb == nullptr) {
        CAMERA_VDI_LOGW("OnFrameAvailable(%{public}s): RequestBuffer rc=%{public}d "
                        "(transient — dropping frame)",
                        ohosCameraId_.c_str(), (int)rc);
        loader.BufferUnlock(buf);
        loader.BufferRelease(buf, nullptr, nullptr);
        return true;
    }

    bool copied = CopyYuvToSurfaceBuffer(info, yuv, sb.GetRefPtr());
    loader.BufferUnlock(buf);
    loader.BufferRelease(buf, nullptr, nullptr);

    if (!copied) {
        surface->CancelBuffer(sb);
        return true;
    }

    BufferFlushConfig flushCfg{};
    flushCfg.damage.w   = info.width;
    flushCfg.damage.h   = info.height;
    flushCfg.timestamp  = info.timestamp;
    fence = -1;
    auto frc = surface->FlushBuffer(sb, fence, flushCfg);
    if (frc != OHOS::GSERROR_OK) {
        CAMERA_VDI_LOGW("OnFrameAvailable(%{public}s): FlushBuffer rc=%{public}d",
                        ohosCameraId_.c_str(), (int)frc);
        return true;
    }

    if (cb != nullptr && captureId >= 0) {
        std::vector<int32_t> ids{streamId};
        cb->OnFrameShutter(captureId, ids,
                           static_cast<uint64_t>(info.timestamp));
    }
    return true;
}

bool HybrisStreamOperatorVdiImpl::CopyYuvToSurfaceBuffer(
    const DroidMediaBufferInfo &info, const DroidMediaBufferYCbCr &yuv,
    OHOS::SurfaceBuffer *out)
{
    if (out == nullptr || out->GetVirAddr() == nullptr) {
        return false;
    }
    const int32_t w   = static_cast<int32_t>(info.width);
    const int32_t h   = static_cast<int32_t>(info.height);
    const int32_t dst_stride = out->GetStride();

    uint8_t *dst       = static_cast<uint8_t *>(out->GetVirAddr());
    const uint8_t *src = static_cast<const uint8_t *>(yuv.y);

    /* Y plane: per-row memcpy honouring HAL ystride vs dst stride. */
    for (int32_t row = 0; row < h; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * dst_stride,
                    src + static_cast<size_t>(row) * yuv.ystride,
                    static_cast<size_t>(w));
    }

    /* UV plane: NV12 layout has Cb before Cr (yuv.cb < yuv.cr),
     * NV21 has Cr before Cb (yuv.cr < yuv.cb).  OHOS GRAPHIC_PIXEL_FMT
     * _YCRCB_420_SP is NV21 → if HAL gave us NV12 we have to swap U/V
     * during the copy.  chroma_step is the stride between adjacent U
     * (or V) samples — 2 for both NV12 and NV21. */
    uint8_t *dst_uv       = dst + static_cast<size_t>(h) * dst_stride;
    const int32_t uv_rows = h / 2;
    const bool srcIsNv21  = (yuv.cr < yuv.cb);

    if (yuv.chroma_step == 2 && srcIsNv21) {
        /* Source NV21, target NV21 — single memcpy per UV row. */
        const uint8_t *src_uv = static_cast<const uint8_t *>(yuv.cr);
        for (int32_t row = 0; row < uv_rows; ++row) {
            std::memcpy(dst_uv + static_cast<size_t>(row) * dst_stride,
                        src_uv + static_cast<size_t>(row) * yuv.cstride,
                        static_cast<size_t>(w));
        }
    } else if (yuv.chroma_step == 2 && !srcIsNv21) {
        /* Source NV12 (Cb first), target NV21 → swap byte order per
         * sample so dst pairs become (Cr, Cb). */
        const uint8_t *src_cb = static_cast<const uint8_t *>(yuv.cb);
        const uint8_t *src_cr = static_cast<const uint8_t *>(yuv.cr);
        for (int32_t row = 0; row < uv_rows; ++row) {
            uint8_t *d = dst_uv + static_cast<size_t>(row) * dst_stride;
            const uint8_t *scb = src_cb + static_cast<size_t>(row) * yuv.cstride;
            const uint8_t *scr = src_cr + static_cast<size_t>(row) * yuv.cstride;
            for (int32_t col = 0; col < w; col += 2) {
                d[col]     = scr[col];
                d[col + 1] = scb[col];
            }
        }
    } else {
        /* Planar I420 (chroma_step=1) or unknown — interleave U/V into
         * a single NV21 plane.  Slow but correct for any layout the
         * HAL might emit. */
        const uint8_t *src_cb = static_cast<const uint8_t *>(yuv.cb);
        const uint8_t *src_cr = static_cast<const uint8_t *>(yuv.cr);
        const int32_t uv_w = w / 2;
        for (int32_t row = 0; row < uv_rows; ++row) {
            uint8_t *d = dst_uv + static_cast<size_t>(row) * dst_stride;
            const uint8_t *scb = src_cb + static_cast<size_t>(row) * yuv.cstride;
            const uint8_t *scr = src_cr + static_cast<size_t>(row) * yuv.cstride;
            for (int32_t col = 0; col < uv_w; ++col) {
                d[col * 2]     = scr[static_cast<size_t>(col) * yuv.chroma_step];
                d[col * 2 + 1] = scb[static_cast<size_t>(col) * yuv.chroma_step];
            }
        }
    }
    return true;
}

void HybrisStreamOperatorVdiImpl::OnShutter()
{
    /* Notify the framework that the shutter has fired (timestamp ≈ now
     * — Camera1 doesn't expose a per-shot timestamp through this
     * callback).  The OnCompressedImage callback below carries the
     * actual JPEG and triggers OnCaptureEnded. */
    sptr<IStreamOperatorVdiCallback> cb;
    int32_t captureId = -1;
    int32_t streamId  = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb        = streamCallback_;
        captureId = jpegCaptureId_;
        streamId  = jpegStreamId_;
    }
    if (cb != nullptr && captureId >= 0 && streamId >= 0) {
        std::vector<int32_t> ids{streamId};
        cb->OnFrameShutter(captureId, ids,
                           static_cast<uint64_t>(/*nsecs*/ 0));
    }
}

void HybrisStreamOperatorVdiImpl::OnCompressedImage(const DroidMediaData *mem)
{
    if (mem == nullptr || mem->data == nullptr || mem->size <= 0) {
        return;
    }
    sptr<OHOS::Surface>             surface;
    sptr<IStreamOperatorVdiCallback> cb;
    int32_t captureId = -1;
    int32_t streamId  = -1;
    BufferRequestConfig             cfg{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        captureId = jpegCaptureId_;
        streamId  = jpegStreamId_;
        cb        = streamCallback_;
        if (streamId >= 0) {
            auto it = streams_.find(streamId);
            if (it != streams_.end()) {
                surface = it->second.surface;
                cfg     = it->second.requestConfig;
            }
        }
        /* Single-shot: clear captureId so a stray duplicate compressed
         * callback doesn't fire twice. */
        jpegCaptureId_ = -1;
    }

    if (surface == nullptr) {
        CAMERA_VDI_LOGW("OnCompressedImage(%{public}s): no jpeg surface "
                        "(streamId=%{public}d captureId=%{public}d) — JPEG dropped",
                        ohosCameraId_.c_str(), streamId, captureId);
        return;
    }
    /* BLOB buffers are 1D byte arrays; request a buffer the size of the
     * JPEG (or the registered BLOB max, whichever is larger). */
    BufferRequestConfig req = cfg;
    req.width  = static_cast<int32_t>(mem->size);
    req.height = 1;
    req.format = kGraphicPixelFmtBLOB;

    sptr<OHOS::SurfaceBuffer> sb;
    int32_t fence = -1;
    auto rc = surface->RequestBuffer(sb, fence, req);
    if (fence >= 0) {
        ::close(fence);
    }
    if (rc != OHOS::GSERROR_OK || sb == nullptr) {
        CAMERA_VDI_LOGE("OnCompressedImage(%{public}s): RequestBuffer rc=%{public}d "
                        "size=%{public}zd", ohosCameraId_.c_str(), (int)rc,
                        (ssize_t)mem->size);
        return;
    }
    auto dstSize = static_cast<size_t>(sb->GetSize());
    auto copySize = std::min(dstSize, static_cast<size_t>(mem->size));
    std::memcpy(sb->GetVirAddr(), mem->data, copySize);

    BufferFlushConfig flushCfg{};
    flushCfg.damage.w   = req.width;
    flushCfg.damage.h   = req.height;
    flushCfg.timestamp  = 0;
    fence = -1;
    auto frc = surface->FlushBuffer(sb, fence, flushCfg);
    if (frc != OHOS::GSERROR_OK) {
        CAMERA_VDI_LOGE("OnCompressedImage(%{public}s): FlushBuffer rc=%{public}d",
                        ohosCameraId_.c_str(), (int)frc);
        return;
    }

    CAMERA_VDI_LOGI("OnCompressedImage(%{public}s): JPEG %{public}zd B delivered "
                    "(captureId=%{public}d streamId=%{public}d)",
                    ohosCameraId_.c_str(), (ssize_t)mem->size, captureId, streamId);

    if (cb != nullptr && captureId >= 0 && streamId >= 0) {
        std::vector<V::VdiCaptureEndedInfo> infos;
        V::VdiCaptureEndedInfo end{};
        end.streamId_  = streamId;
        end.frameCount_ = 1;
        infos.push_back(end);
        cb->OnCaptureEnded(captureId, infos);
    }
}

} // namespace OHOS::Camera::Hybris
