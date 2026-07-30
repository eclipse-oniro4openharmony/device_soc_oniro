/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hybris_stream_operator_vdi.h"

#include <cstring>
#include <unistd.h>

#include "surface_type.h"
#include "surface_buffer.h"
#include "video_key_info.h"

#include "hybris_camera_common.h"
#include "hybris_camera_device_vdi.h"

namespace OHOS {
namespace HybrisCamera {
namespace {

// Buffers requested for a BLOB stream are linear; the reference VDI asks for a
// fixed 24 MB slab because the encoded size is unknown until the frame lands.
constexpr int32_t BLOB_BUFFER_BYTES = 24 * 1024 * 1024;
constexpr int32_t STRIDE_ALIGNMENT = 8;
constexpr int32_t REQUEST_TIMEOUT_MS = 0;
constexpr int32_t READER_MAX_IMAGES = 4;
constexpr int32_t BLOB_READER_MAX_IMAGES = 2;

bool IsBlobFormat(int32_t pixelFormat)
{
    return pixelFormat == GRAPHIC_PIXEL_FMT_BLOB;
}

// Trims the padding the HAL leaves after the JPEG's EOI marker so the photo
// consumer is told the true encoded length.
int32_t JpegLength(const uint8_t *data, int32_t len)
{
    constexpr int32_t MIN_JPEG = 4;
    if (data == nullptr || len < MIN_JPEG) {
        return len;
    }
    for (int32_t i = len - 2; i >= 2; --i) {
        if (data[i] == 0xff && data[i + 1] == 0xd9) {
            return i + 2;
        }
    }
    return len;
}

} // namespace

HybrisStreamOperator::HybrisStreamOperator(const NdkApi *ndk, HybrisCameraDevice *device,
                                           const sptr<IStreamOperatorVdiCallback> &callback)
    : ndk_(ndk), device_(device), callback_(callback)
{
}

HybrisStreamOperator::~HybrisStreamOperator()
{
    Shutdown();
}

int32_t HybrisStreamOperator::IsStreamsSupported(VdiOperationMode /*mode*/,
                                                 const std::vector<uint8_t> & /*modeSetting*/,
                                                 const std::vector<VdiStreamInfo> &infos,
                                                 VdiStreamSupportType &type)
{
    // camera2 negotiates the real combination at createCaptureSession; anything
    // we would reject here the HAL would reject there with a clearer error.
    type = infos.empty() ? NOT_SUPPORTED : DYNAMIC_SUPPORTED;
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisStreamOperator::CreateStreams(const std::vector<VdiStreamInfo> &streamInfos)
{
    std::lock_guard<std::mutex> guard(lock_);
    for (const auto &info : streamInfos) {
        if (streams_.count(info.streamId_) != 0) {
            HC_LOGW("stream %{public}d already exists", info.streamId_);
            continue;
        }
        auto stream = std::make_shared<Stream>();
        stream->info = info;
        stream->isBlob = IsBlobFormat(info.format_);

        if (info.bufferQueue_ == nullptr || info.bufferQueue_->producer_ == nullptr) {
            HC_LOGE("stream %{public}d has no buffer producer", info.streamId_);
            return VDI::Camera::V1_0::INVALID_ARGUMENT;
        }
        stream->surface = OHOS::Surface::CreateSurfaceAsProducer(info.bufferQueue_->producer_);
        if (stream->surface == nullptr) {
            HC_LOGE("stream %{public}d: CreateSurfaceAsProducer failed", info.streamId_);
            return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
        }

        int32_t readerFormat = stream->isBlob ? AIMAGE_FORMAT_JPEG : AIMAGE_FORMAT_YUV_420_888;
        int32_t maxImages = stream->isBlob ? BLOB_READER_MAX_IMAGES : READER_MAX_IMAGES;
        media_status_t ms = ndk_->AImageReader_new(info.width_, info.height_, readerFormat,
                                                   maxImages, &stream->reader);
        if (ms != AMEDIA_OK || stream->reader == nullptr) {
            HC_LOGE("stream %{public}d: AImageReader_new(%{public}dx%{public}d fmt=0x%{public}x) "
                    "failed: %{public}d",
                    info.streamId_, info.width_, info.height_, readerFormat, ms);
            return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
        }

        stream->listenerCtx = std::make_unique<ListenerCtx>();
        stream->listenerCtx->self = this;
        stream->listenerCtx->streamId = info.streamId_;
        AImageReader_ImageListener listener { stream->listenerCtx.get(), OnImageAvailable };
        ndk_->AImageReader_setImageListener(stream->reader, &listener);

        ndk_->AImageReader_getWindow(stream->reader, &stream->window);
        if (stream->window == nullptr) {
            HC_LOGE("stream %{public}d: AImageReader_getWindow returned null", info.streamId_);
            return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
        }
        streams_[info.streamId_] = stream;
        HC_LOGI("created stream %{public}d %{public}dx%{public}d fmt=%{public}d intent=%{public}d "
                "blob=%{public}d",
                info.streamId_, info.width_, info.height_, info.format_,
                static_cast<int>(info.intent_), stream->isBlob ? 1 : 0);
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

void HybrisStreamOperator::DestroyStream(const std::shared_ptr<Stream> &stream)
{
    if (stream->target != nullptr) {
        ndk_->ACameraOutputTarget_free(stream->target);
        stream->target = nullptr;
    }
    if (stream->output != nullptr) {
        // The container itself is rebuilt from scratch on every commit, so the
        // output only has to be freed, never unlinked.
        ndk_->ACaptureSessionOutput_free(stream->output);
        stream->output = nullptr;
    }
    if (stream->reader != nullptr) {
        ndk_->AImageReader_setImageListener(stream->reader, nullptr);
        ndk_->AImageReader_delete(stream->reader);
        stream->reader = nullptr;
        stream->window = nullptr;
    }
    stream->listenerCtx.reset(); // safe now: the reader can no longer call us
    stream->surface = nullptr;
}

void HybrisStreamOperator::CloseSession(ACameraCaptureSession *session, ACaptureRequest *repeating,
                                        ACaptureSessionOutputContainer *outputs)
{
    if (session != nullptr) {
        ndk_->ACameraCaptureSession_stopRepeating(session);
        ndk_->ACameraCaptureSession_close(session);
    }
    if (repeating != nullptr) {
        ndk_->ACaptureRequest_free(repeating);
    }
    if (outputs != nullptr) {
        ndk_->ACaptureSessionOutputContainer_free(outputs);
    }
}

int32_t HybrisStreamOperator::ReleaseStreams(const std::vector<int32_t> &streamIds)
{
    std::vector<std::shared_ptr<Stream>> doomed;
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (int32_t id : streamIds) {
            auto it = streams_.find(id);
            if (it == streams_.end()) {
                continue;
            }
            doomed.push_back(it->second);
            streams_.erase(it);
        }
    }
    for (const auto &stream : doomed) {
        DestroyStream(stream);
        HC_LOGI("released stream %{public}d", stream->info.streamId_);
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisStreamOperator::CommitStreams(VdiOperationMode /*mode*/,
                                            const std::vector<uint8_t> &modeSetting)
{
    // A camera2 session is immutable, so a re-commit means tearing the old one
    // down first — with the lock released (see CloseSession).  The framework
    // does this whenever the stream set changes.
    {
        ACameraCaptureSession *oldSession = nullptr;
        ACaptureRequest *oldRepeating = nullptr;
        ACaptureSessionOutputContainer *oldOutputs = nullptr;
        {
            std::lock_guard<std::mutex> guard(lock_);
            std::swap(oldSession, session_);
            std::swap(oldRepeating, repeatingRequest_);
            std::swap(oldOutputs, outputs_);
            committed_ = false;
        }
        CloseSession(oldSession, oldRepeating, oldOutputs);
    }

    std::lock_guard<std::mutex> guard(lock_);
    if (device_ == nullptr || device_->NdkDevice() == nullptr) {
        return VDI::Camera::V1_0::CAMERA_CLOSED;
    }
    if (streams_.empty()) {
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }
    ndk_->ACaptureSessionOutputContainer_create(&outputs_);
    if (outputs_ == nullptr) {
        return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
    }
    for (auto &entry : streams_) {
        auto &stream = entry.second;
        if (stream->output != nullptr) {
            ndk_->ACaptureSessionOutput_free(stream->output);
            stream->output = nullptr;
        }
        camera_status_t st = ndk_->ACaptureSessionOutput_create(stream->window, &stream->output);
        if (st != ACAMERA_OK) {
            HC_LOGE("stream %{public}d: ACaptureSessionOutput_create -> %{public}s", entry.first,
                    NdkStatusName(st));
            return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
        }
        ndk_->ACaptureSessionOutputContainer_add(outputs_, stream->output);
        if (stream->target == nullptr) {
            ndk_->ACameraOutputTarget_create(stream->window, &stream->target);
        }
    }

    ACameraCaptureSession_stateCallbacks stateCb {};
    stateCb.context = this;
    stateCb.onClosed = OnSessionClosed;
    stateCb.onReady = OnSessionReady;
    stateCb.onActive = OnSessionActive;

    camera_status_t st = ndk_->ACameraDevice_createCaptureSession(device_->NdkDevice(), outputs_,
                                                                 &stateCb, &session_);
    if (st != ACAMERA_OK || session_ == nullptr) {
        HC_LOGE("createCaptureSession -> %{public}s (%{public}zu streams)", NdkStatusName(st),
                streams_.size());
        return VDI::Camera::V1_0::DEVICE_ERROR;
    }
    committed_ = true;
    HC_LOGI("committed %{public}zu stream(s), modeSetting=%{public}zu bytes", streams_.size(),
            modeSetting.size());
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisStreamOperator::GetStreamAttributes(std::vector<VdiStreamAttribute> &attributes)
{
    std::lock_guard<std::mutex> guard(lock_);
    attributes.clear();
    for (const auto &entry : streams_) {
        const auto &stream = entry.second;
        VdiStreamAttribute attr {};
        attr.streamId_ = stream->info.streamId_;
        attr.width_ = stream->info.width_;
        attr.height_ = stream->info.height_;
        attr.overrideFormat_ = stream->info.format_;
        attr.overrideDataspace_ = stream->info.dataspace_;
        attr.producerUsage_ = static_cast<int32_t>(BUFFER_USAGE_CPU_READ | BUFFER_USAGE_CPU_WRITE |
                                                  BUFFER_USAGE_MEM_DMA);
        attr.producerBufferCount_ = stream->isBlob ? BLOB_READER_MAX_IMAGES : READER_MAX_IMAGES;
        attr.maxBatchCaptureCount_ = 1;
        attr.maxCaptureCount_ = 1;
        attributes.push_back(attr);
    }
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisStreamOperator::AttachBufferQueue(int32_t streamId,
                                                const sptr<BufferProducerSequenceable> &bufferProducer)
{
    std::lock_guard<std::mutex> guard(lock_);
    auto it = streams_.find(streamId);
    if (it == streams_.end() || bufferProducer == nullptr ||
        bufferProducer->producer_ == nullptr) {
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }
    it->second->surface = OHOS::Surface::CreateSurfaceAsProducer(bufferProducer->producer_);
    return it->second->surface != nullptr ? VDI::Camera::V1_0::NO_ERROR
                                          : VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
}

int32_t HybrisStreamOperator::DetachBufferQueue(int32_t streamId)
{
    std::lock_guard<std::mutex> guard(lock_);
    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }
    it->second->surface = nullptr;
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisStreamOperator::Capture(int32_t captureId, const VdiCaptureInfo &info, bool isStreaming)
{
    std::vector<int32_t> targets;
    int32_t rc = SubmitCapture(captureId, info, isStreaming, targets);
    if (rc != VDI::Camera::V1_0::NO_ERROR) {
        return rc;
    }
    // Outside the lock: this is a binder call back into the framework.
    sptr<IStreamOperatorVdiCallback> cb;
    {
        std::lock_guard<std::mutex> guard(lock_);
        cb = callback_;
    }
    if (cb != nullptr) {
        cb->OnCaptureStarted(captureId, targets);
    }
    HC_LOGI("capture %{public}d started (streaming=%{public}d, %{public}zu stream(s))", captureId,
            isStreaming ? 1 : 0, targets.size());
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisStreamOperator::SubmitCapture(int32_t captureId, const VdiCaptureInfo &info,
                                            bool isStreaming, std::vector<int32_t> &targets)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (!committed_ || session_ == nullptr || device_ == nullptr) {
        HC_LOGE("capture %{public}d before a session exists", captureId);
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }

    ACaptureRequest *request = nullptr;
    ACameraDevice_request_template tmpl = isStreaming ? TEMPLATE_PREVIEW : TEMPLATE_STILL_CAPTURE;
    camera_status_t st = ndk_->ACameraDevice_createCaptureRequest(device_->NdkDevice(), tmpl,
                                                                  &request);
    if (st != ACAMERA_OK || request == nullptr) {
        HC_LOGE("createCaptureRequest -> %{public}s", NdkStatusName(st));
        return VDI::Camera::V1_0::DEVICE_ERROR;
    }

    for (int32_t streamId : info.streamIds_) {
        auto it = streams_.find(streamId);
        if (it == streams_.end() || it->second->target == nullptr) {
            HC_LOGE("capture %{public}d references unknown stream %{public}d", captureId, streamId);
            ndk_->ACaptureRequest_free(request);
            return VDI::Camera::V1_0::INVALID_ARGUMENT;
        }
        ndk_->ACaptureRequest_addTarget(request, it->second->target);
        it->second->captureId = captureId;
        it->second->streaming = isStreaming;
        it->second->shutterCallback = info.enableShutterCallback_;
        it->second->frameCount = 0;
        targets.push_back(streamId);
    }

    // Device-wide settings first (UpdateSettings), then whatever this capture
    // carries — the per-capture settings win, matching the OHOS contract.
    if (device_ != nullptr) {
        ApplySettingsToRequest(ndk_, device_->LatestSettings(), *device_->Info(), request);
    }
    ApplySettingsToRequest(ndk_, info.captureSetting_, *device_->Info(), request);

    ACameraCaptureSession_captureCallbacks capCb {};
    capCb.context = this;
    capCb.onCaptureStarted = OnCaptureStartedCb;
    capCb.onCaptureProgressed = nullptr;
    capCb.onCaptureCompleted = OnCaptureCompletedCb;
    capCb.onCaptureFailed = OnCaptureFailedCb;
    capCb.onCaptureSequenceCompleted = OnSequenceCompletedCb;
    capCb.onCaptureSequenceAborted = OnSequenceAbortedCb;
    capCb.onCaptureBufferLost = OnBufferLostCb;

    if (isStreaming) {
        if (repeatingRequest_ != nullptr) {
            ndk_->ACameraCaptureSession_stopRepeating(session_);
            ndk_->ACaptureRequest_free(repeatingRequest_);
            repeatingRequest_ = nullptr;
        }
        st = ndk_->ACameraCaptureSession_setRepeatingRequest(session_, &capCb, 1, &request, nullptr);
        if (st != ACAMERA_OK) {
            HC_LOGE("setRepeatingRequest -> %{public}s", NdkStatusName(st));
            ndk_->ACaptureRequest_free(request);
            return VDI::Camera::V1_0::DEVICE_ERROR;
        }
        repeatingRequest_ = request; // freed when the repeat is cancelled
    } else {
        st = ndk_->ACameraCaptureSession_capture(session_, &capCb, 1, &request, nullptr);
        ndk_->ACaptureRequest_free(request);
        if (st != ACAMERA_OK) {
            HC_LOGE("capture -> %{public}s", NdkStatusName(st));
            return VDI::Camera::V1_0::DEVICE_ERROR;
        }
    }

    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisStreamOperator::CancelCapture(int32_t captureId)
{
    std::vector<VdiCaptureEndedInfo> ended;
    ACameraCaptureSession *stopSession = nullptr;
    ACaptureRequest *staleRequest = nullptr;
    sptr<IStreamOperatorVdiCallback> cb;
    {
        std::lock_guard<std::mutex> guard(lock_);
        bool wasStreaming = false;
        for (auto &entry : streams_) {
            auto &stream = entry.second;
            if (stream->captureId != captureId) {
                continue;
            }
            wasStreaming = wasStreaming || stream->streaming;
            VdiCaptureEndedInfo info {};
            info.streamId_ = stream->info.streamId_;
            info.frameCount_ = static_cast<int32_t>(stream->frameCount);
            ended.push_back(info);
            stream->captureId = -1;
            stream->streaming = false;
        }
        if (wasStreaming) {
            stopSession = session_;
            std::swap(staleRequest, repeatingRequest_);
        }
        cb = callback_;
    }
    // stopRepeating waits for capture callbacks, which take lock_.
    if (stopSession != nullptr) {
        ndk_->ACameraCaptureSession_stopRepeating(stopSession);
    }
    if (staleRequest != nullptr) {
        ndk_->ACaptureRequest_free(staleRequest);
    }
    if (cb != nullptr && !ended.empty()) {
        cb->OnCaptureEnded(captureId, ended);
    }
    HC_LOGI("capture %{public}d cancelled (%{public}zu stream(s))", captureId, ended.size());
    return VDI::Camera::V1_0::NO_ERROR;
}

int32_t HybrisStreamOperator::ChangeToOfflineStream(const std::vector<int32_t> & /*streamIds*/,
                                                    const sptr<IStreamOperatorVdiCallback> & /*cb*/,
                                                    sptr<IOfflineStreamOperatorVdi> & /*op*/)
{
    return VDI::Camera::V1_0::METHOD_NOT_SUPPORTED;
}

void HybrisStreamOperator::Shutdown()
{
    ACameraCaptureSession *session = nullptr;
    ACaptureRequest *repeating = nullptr;
    ACaptureSessionOutputContainer *outputs = nullptr;
    std::map<int32_t, std::shared_ptr<Stream>> streams;
    {
        std::lock_guard<std::mutex> guard(lock_);
        std::swap(session, session_);
        std::swap(repeating, repeatingRequest_);
        std::swap(outputs, outputs_);
        streams.swap(streams_);
        committed_ = false;
    }
    // Session first: it must stop feeding the readers before they are deleted.
    CloseSession(session, repeating, outputs);
    for (auto &entry : streams) {
        DestroyStream(entry.second);
    }
}

std::shared_ptr<HybrisStreamOperator::Stream> HybrisStreamOperator::FindStream(int32_t streamId)
{
    auto it = streams_.find(streamId);
    return it == streams_.end() ? nullptr : it->second;
}

// ─── frame delivery ─────────────────────────────────────────────────────────

void HybrisStreamOperator::OnImageAvailable(void *context, AImageReader *reader)
{
    auto *ctx = static_cast<ListenerCtx *>(context);
    if (ctx != nullptr && ctx->self != nullptr) {
        ctx->self->HandleImage(ctx->streamId, reader);
    }
}

void HybrisStreamOperator::HandleImage(int32_t streamId, AImageReader *reader)
{
    AImage *image = nullptr;
    if (ndk_->AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK || image == nullptr) {
        return;
    }
    int64_t timestamp = 0;
    ndk_->AImage_getTimestamp(image, &timestamp);

    std::shared_ptr<Stream> stream;
    int32_t captureId = -1;
    bool shutter = false;
    bool streaming = false;
    bool firstImage = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        stream = FindStream(streamId);
        if (stream == nullptr || stream->surface == nullptr) {
            ndk_->AImage_delete(image);
            return;
        }
        captureId = stream->captureId;
        shutter = stream->shutterCallback;
        streaming = stream->streaming;
        firstImage = stream->frameCount == 0;
    }

    bool delivered = stream->isBlob ? DeliverBlob(*stream, image, timestamp)
                                    : DeliverYuv(*stream, image, timestamp);
    if (firstImage) {
        int32_t w = 0;
        int32_t h = 0;
        int32_t fmt = 0;
        ndk_->AImage_getWidth(image, &w);
        ndk_->AImage_getHeight(image, &h);
        ndk_->AImage_getFormat(image, &fmt);
        HC_LOGI("stream %{public}d: first image %{public}dx%{public}d fmt=0x%{public}x "
                "delivered=%{public}d captureId=%{public}d",
                streamId, w, h, fmt, delivered ? 1 : 0, captureId);
    }
    ndk_->AImage_delete(image);
    if (!delivered || captureId < 0) {
        return;
    }

    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> guard(lock_);
        stream->frameCount++;
        count = stream->frameCount;
    }
    if (callback_ == nullptr) {
        return;
    }
    if (shutter) {
        callback_->OnFrameShutter(captureId, { streamId }, static_cast<uint64_t>(timestamp));
    }
    if (!streaming) {
        // A one-shot capture is only complete once its buffer has been handed
        // to the consumer; without this the photo path waits forever.
        VdiCaptureEndedInfo ended {};
        ended.streamId_ = streamId;
        ended.frameCount_ = static_cast<int32_t>(count);
        callback_->OnCaptureEnded(captureId, { ended });
        std::lock_guard<std::mutex> guard(lock_);
        stream->captureId = -1;
    }
}

bool HybrisStreamOperator::DeliverYuv(Stream &stream, AImage *image, int64_t timestamp)
{
    int32_t planeCount = 0;
    ndk_->AImage_getNumberOfPlanes(image, &planeCount);
    constexpr int32_t YUV_PLANES = 3;
    if (planeCount < YUV_PLANES) {
        HC_LOGE("stream %{public}d: expected 3 planes, got %{public}d", stream.info.streamId_,
                planeCount);
        return false;
    }
    int32_t width = 0;
    int32_t height = 0;
    ndk_->AImage_getWidth(image, &width);
    ndk_->AImage_getHeight(image, &height);

    uint8_t *y = nullptr;
    uint8_t *u = nullptr;
    uint8_t *v = nullptr;
    int yLen = 0;
    int uLen = 0;
    int vLen = 0;
    int32_t yStride = 0;
    int32_t uStride = 0;
    int32_t uPixStride = 0;
    ndk_->AImage_getPlaneData(image, 0, &y, &yLen);
    ndk_->AImage_getPlaneData(image, 1, &u, &uLen);
    ndk_->AImage_getPlaneData(image, 2, &v, &vLen);
    ndk_->AImage_getPlaneRowStride(image, 0, &yStride);
    ndk_->AImage_getPlaneRowStride(image, 1, &uStride);
    ndk_->AImage_getPlanePixelStride(image, 1, &uPixStride);
    if (y == nullptr || u == nullptr || v == nullptr) {
        return false;
    }

    OHOS::sptr<OHOS::SurfaceBuffer> sb = nullptr;
    int32_t fence = -1;
    OHOS::BufferRequestConfig config {};
    config.width = stream.info.width_;
    config.height = stream.info.height_;
    config.format = stream.info.format_;
    config.strideAlignment = STRIDE_ALIGNMENT;
    config.usage = BUFFER_USAGE_CPU_READ | BUFFER_USAGE_CPU_WRITE | BUFFER_USAGE_MEM_DMA;
    config.timeout = REQUEST_TIMEOUT_MS;
    OHOS::SurfaceError err = stream.surface->RequestBuffer(sb, fence, config);
    if (fence >= 0) {
        close(fence);
    }
    if (err != OHOS::SURFACE_ERROR_OK || sb == nullptr) {
        // Dropping is the right answer: blocking here stalls the Android
        // reader and the HAL starts losing buffers instead.
        if (stream.frameCount == 0) {
            HC_LOGE("stream %{public}d: RequestBuffer(%{public}dx%{public}d fmt=%{public}d) "
                    "failed: %{public}d", stream.info.streamId_, config.width, config.height,
                    config.format, err);
        }
        return false;
    }

    auto *dst = static_cast<uint8_t *>(sb->GetVirAddr());
    int32_t dstStride = sb->GetStride();
    if (dst == nullptr || dstStride <= 0) {
        stream.surface->CancelBuffer(sb);
        return false;
    }
    int32_t copyWidth = std::min(width, dstStride);
    // Never trust the geometry over the allocation: a buffer sized for luma
    // only would otherwise have its chroma written past the end of the mapping.
    const size_t dstBytes = sb->GetSize();
    const size_t lumaBytes = static_cast<size_t>(dstStride) * height;
    const size_t neededBytes = lumaBytes + lumaBytes / 2;
    if (dstBytes < neededBytes) {
        if (stream.frameCount == 0) {
            HC_LOGE("stream %{public}d: buffer is %{public}zu bytes but NV21 %{public}dx%{public}d "
                    "needs %{public}zu — dropping frames rather than overrunning it",
                    stream.info.streamId_, dstBytes, dstStride, height, neededBytes);
        }
        stream.surface->CancelBuffer(sb);
        return false;
    }
    for (int32_t row = 0; row < height; ++row) {
        (void)memcpy(dst + static_cast<size_t>(row) * dstStride,
                     y + static_cast<size_t>(row) * yStride, static_cast<size_t>(copyWidth));
    }

    // NV21 chroma: V and U interleaved, V first.  When the HAL hands us a
    // semi-planar buffer in that order (v == u - 1, the MT6878 case) the whole
    // plane is one memcpy per row; otherwise interleave by hand.
    uint8_t *dstChroma = dst + static_cast<size_t>(dstStride) * height;
    int32_t chromaRows = height / 2;
    if (uPixStride == 2 && v + 1 == u) {
        for (int32_t row = 0; row < chromaRows; ++row) {
            (void)memcpy(dstChroma + static_cast<size_t>(row) * dstStride,
                         v + static_cast<size_t>(row) * uStride, static_cast<size_t>(copyWidth));
        }
    } else {
        int32_t chromaCols = copyWidth / 2;
        for (int32_t row = 0; row < chromaRows; ++row) {
            uint8_t *out = dstChroma + static_cast<size_t>(row) * dstStride;
            const uint8_t *srcV = v + static_cast<size_t>(row) * uStride;
            const uint8_t *srcU = u + static_cast<size_t>(row) * uStride;
            for (int32_t col = 0; col < chromaCols; ++col) {
                out[col * 2] = srcV[col * uPixStride];
                out[col * 2 + 1] = srcU[col * uPixStride];
            }
        }
    }

    const OHOS::sptr<OHOS::BufferExtraData> &extra = sb->GetExtraData();
    if (extra != nullptr) {
        extra->ExtraSet(OHOS::Camera::dataSize, static_cast<int32_t>(neededBytes));
        extra->ExtraSet(OHOS::Camera::timeStamp, timestamp);
        extra->ExtraSet(OHOS::Camera::streamId, stream.info.streamId_);
        extra->ExtraSet(OHOS::Camera::captureId, stream.captureId);
        extra->ExtraSet(OHOS::Camera::dataWidth, stream.info.width_);
        extra->ExtraSet(OHOS::Camera::dataHeight, stream.info.height_);
        extra->ExtraSet(OHOS::Camera::isKeyFrame, 1);
    }
    OHOS::BufferFlushConfig flush {};
    flush.damage.x = 0;
    flush.damage.y = 0;
    flush.damage.w = stream.info.width_;
    flush.damage.h = stream.info.height_;
    flush.timestamp = timestamp;
    stream.surface->FlushBuffer(sb, -1, flush);
    return true;
}

bool HybrisStreamOperator::DeliverBlob(Stream &stream, AImage *image, int64_t timestamp)
{
    uint8_t *data = nullptr;
    int len = 0;
    ndk_->AImage_getPlaneData(image, 0, &data, &len);
    if (data == nullptr || len <= 0) {
        return false;
    }
    int32_t jpegLen = JpegLength(data, len);

    OHOS::sptr<OHOS::SurfaceBuffer> sb = nullptr;
    int32_t fence = -1;
    OHOS::BufferRequestConfig config {};
    // BLOB buffers are linear; the encoded size is only known now, so ask for a
    // slab that any still can fit into (the reference VDI does the same).
    config.width = BLOB_BUFFER_BYTES;
    config.height = 1;
    config.format = stream.info.format_;
    config.strideAlignment = STRIDE_ALIGNMENT;
    config.usage = BUFFER_USAGE_CPU_READ | BUFFER_USAGE_CPU_WRITE | BUFFER_USAGE_MEM_DMA;
    config.timeout = REQUEST_TIMEOUT_MS;
    OHOS::SurfaceError err = stream.surface->RequestBuffer(sb, fence, config);
    if (fence >= 0) {
        close(fence);
    }
    if (err != OHOS::SURFACE_ERROR_OK || sb == nullptr) {
        HC_LOGE("stream %{public}d: RequestBuffer for a %{public}d-byte still failed: %{public}d",
                stream.info.streamId_, jpegLen, err);
        return false;
    }
    auto *dst = static_cast<uint8_t *>(sb->GetVirAddr());
    if (dst == nullptr) {
        stream.surface->CancelBuffer(sb);
        return false;
    }
    int32_t copyLen = std::min<int32_t>(jpegLen, static_cast<int32_t>(sb->GetSize()));
    (void)memcpy(dst, data, static_cast<size_t>(copyLen));

    const OHOS::sptr<OHOS::BufferExtraData> &extra = sb->GetExtraData();
    if (extra != nullptr) {
        // dataSize is what the photo consumer reads to find the JPEG's length.
        extra->ExtraSet(OHOS::Camera::dataSize, copyLen);
        extra->ExtraSet(OHOS::Camera::timeStamp, timestamp);
        extra->ExtraSet(OHOS::Camera::streamId, stream.info.streamId_);
        extra->ExtraSet(OHOS::Camera::captureId, stream.captureId);
        extra->ExtraSet(OHOS::Camera::dataWidth, stream.info.width_);
        extra->ExtraSet(OHOS::Camera::dataHeight, stream.info.height_);
        extra->ExtraSet(OHOS::Camera::isKeyFrame, 1);
        extra->ExtraSet(OHOS::Camera::deferredImageFormat, 1);
    }
    OHOS::BufferFlushConfig flush {};
    flush.damage.w = BLOB_BUFFER_BYTES;
    flush.damage.h = 1;
    flush.timestamp = timestamp;
    stream.surface->FlushBuffer(sb, -1, flush);
    HC_LOGI("stream %{public}d: delivered %{public}d-byte still (raw blob %{public}d)",
            stream.info.streamId_, copyLen, len);
    return true;
}

// ─── camera2 session/capture callbacks ──────────────────────────────────────

void HybrisStreamOperator::OnSessionClosed(void * /*context*/, ACameraCaptureSession * /*session*/)
{
    HC_LOGI("capture session closed");
}

void HybrisStreamOperator::OnSessionReady(void * /*context*/, ACameraCaptureSession * /*session*/)
{
    HC_LOGI("capture session ready");
}

void HybrisStreamOperator::OnSessionActive(void * /*context*/, ACameraCaptureSession * /*session*/)
{
    HC_LOGI("capture session active");
}

void HybrisStreamOperator::OnCaptureStartedCb(void * /*context*/, ACameraCaptureSession * /*s*/,
                                              const ACaptureRequest * /*request*/,
                                              int64_t /*timestamp*/)
{
    // OHOS's OnCaptureStarted is per-capture, not per-frame; it is already sent
    // synchronously from Capture().
}

void HybrisStreamOperator::OnCaptureCompletedCb(void *context, ACameraCaptureSession * /*session*/,
                                                ACaptureRequest * /*request*/,
                                                const ACameraMetadata *result)
{
    auto *self = static_cast<HybrisStreamOperator *>(context);
    if (self == nullptr || self->device_ == nullptr) {
        return;
    }
    ACameraMetadata_const_entry entry {};
    int64_t timestamp = 0;
    if (self->ndk_->ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_TIMESTAMP, &entry) ==
            ACAMERA_OK && entry.count > 0) {
        timestamp = entry.data.i64[0];
    }
    self->device_->ReportResult(result, timestamp);
}

void HybrisStreamOperator::OnCaptureFailedCb(void *context, ACameraCaptureSession * /*session*/,
                                             ACaptureRequest * /*request*/,
                                             ACameraCaptureFailure *failure)
{
    auto *self = static_cast<HybrisStreamOperator *>(context);
    if (self == nullptr || self->callback_ == nullptr || failure == nullptr) {
        return;
    }
    std::vector<VdiCaptureErrorInfo> infos;
    int32_t captureId = -1;
    {
        std::lock_guard<std::mutex> guard(self->lock_);
        for (const auto &entry : self->streams_) {
            if (entry.second->captureId < 0) {
                continue;
            }
            captureId = entry.second->captureId;
            VdiCaptureErrorInfo info {};
            info.streamId_ = entry.first;
            info.error_ = BUFFER_LOST;
            infos.push_back(info);
        }
    }
    HC_LOGE("capture failed: sequence %{public}d frame %{public}lld", failure->sequenceId,
            static_cast<long long>(failure->frameNumber));
    if (captureId >= 0) {
        self->callback_->OnCaptureError(captureId, infos);
    }
}

void HybrisStreamOperator::OnSequenceCompletedCb(void * /*context*/, ACameraCaptureSession * /*s*/,
                                                 int sequenceId, int64_t /*frameNumber*/)
{
    HC_LOGD("capture sequence %{public}d completed", sequenceId);
}

void HybrisStreamOperator::OnSequenceAbortedCb(void * /*context*/, ACameraCaptureSession * /*s*/,
                                               int sequenceId)
{
    HC_LOGW("capture sequence %{public}d aborted", sequenceId);
}

void HybrisStreamOperator::OnBufferLostCb(void *context, ACameraCaptureSession * /*session*/,
                                          ACaptureRequest * /*request*/,
                                          ACameraWindowType * /*window*/, int64_t frameNumber)
{
    (void)context;
    HC_LOGW("buffer lost for frame %{public}lld", static_cast<long long>(frameNumber));
}

} // namespace HybrisCamera
} // namespace OHOS
