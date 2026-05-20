/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hybris_stream_operator_vdi_impl.h"

#include <sstream>

#include "hidl/hw_binder_client.h"
#include "hidl/hw_camera_device.h"
#include "hybris_camera_log.h"

namespace OHOS::Camera::Hybris {

namespace V = OHOS::VDI::Camera::V1_0;

HybrisStreamOperatorVdiImpl::HybrisStreamOperatorVdiImpl(
    Hidl::HwBinderClient *client, Hidl::HwCameraDevice *device,
    const std::string &ohosCameraId,
    const sptr<IStreamOperatorVdiCallback> &cb)
    : client_(client), device_(device),
      ohosCameraId_(ohosCameraId), streamCallback_(cb)
{
    CAMERA_VDI_LOGI("HybrisStreamOperatorVdiImpl ctor for %{public}s "
                    "(halium device handle=%{public}u)",
                    ohosCameraId_.c_str(),
                    device_ ? device_->Handle() : 0);
}

HybrisStreamOperatorVdiImpl::~HybrisStreamOperatorVdiImpl()
{
    CAMERA_VDI_LOGI("HybrisStreamOperatorVdiImpl dtor for %{public}s",
                    ohosCameraId_.c_str());
}

int32_t HybrisStreamOperatorVdiImpl::IsStreamsSupported(
    VdiOperationMode mode, const std::vector<uint8_t> &modeSetting,
    const std::vector<VdiStreamInfo> &infos, VdiStreamSupportType &type)
{
    (void)mode;
    (void)modeSetting;
    (void)infos;
    /*
     * Without a real configureStreams round-trip we can't tell which
     * combinations the HAL accepts; advertise DYNAMIC_SUPPORTED so the
     * framework proceeds to CreateStreams + CommitStreams where we can
     * at least log the actual config it picks.
     */
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
        streams_[s.streamId_] = StreamRecord{s, nullptr};
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
    for (int32_t id : streamIds) {
        streams_.erase(id);
    }
    CAMERA_VDI_LOGI("ReleaseStreams(%{public}s): %{public}zu streams",
                    ohosCameraId_.c_str(), streamIds.size());
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::CommitStreams(
    VdiOperationMode mode, const std::vector<uint8_t> &modeSetting)
{
    (void)modeSetting;
    /*
     * N12.6.1: bridge to ICameraDevice::open + ICameraDeviceSession
     *           ::configureStreams_3_4 here.  Today we accept all
     *           configurations blindly and just log.
     */
    std::lock_guard<std::mutex> lock(mutex_);
    CAMERA_VDI_LOGI("CommitStreams(%{public}s): mode=%{public}d "
                    "modeSetting=%{public}zuB %{public}zu streams pending "
                    "(N12.6.1 stub: not forwarded to Halium yet)",
                    ohosCameraId_.c_str(), static_cast<int32_t>(mode),
                    modeSetting.size(), streams_.size());
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
        attr.producerUsage_         = 0;
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
    it->second.producer = bufferProducer;
    CAMERA_VDI_LOGI("AttachBufferQueue(%{public}s): streamId=%{public}d "
                    "producer=%{public}p",
                    ohosCameraId_.c_str(), streamId, bufferProducer.GetRefPtr());
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::DetachBufferQueue(int32_t streamId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(streamId);
    if (it != streams_.end()) {
        it->second.producer = nullptr;
    }
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::Capture(int32_t captureId,
                                             const VdiCaptureInfo &info,
                                             bool isStreaming)
{
    /*
     * N12.7: bridge to ICameraDeviceSession::processCaptureRequest via
     *        FMQ + buffer interop (Android native_handle_t ↔ OHOS
     *        BufferHandle).  Today we just log + report failure so the
     *        framework knows no frames will arrive.
     */
    std::ostringstream s;
    s << "captureId=" << captureId << " streams=[";
    for (size_t i = 0; i < info.streamIds_.size(); ++i) {
        if (i) s << ",";
        s << info.streamIds_[i];
    }
    s << "] setting=" << info.captureSetting_.size() << "B "
      << "shutterCb=" << (info.enableShutterCallback_ ? "1" : "0")
      << " streaming=" << (isStreaming ? "1" : "0");
    CAMERA_VDI_LOGW("Capture(%{public}s): %{public}s — N12.7 stub, "
                    "accepting request but not forwarding to Halium yet "
                    "(no frames will arrive on the surface)",
                    ohosCameraId_.c_str(), s.str().c_str());
    /*
     * Return NO_ERROR so the OHOS framework believes streaming has
     * started and doesn't enter an error/retry loop.  Frames don't
     * actually flow yet — preview surface will stay black until the
     * Halium ICameraDeviceSession bridge + buffer interop lands in
     * N12.7.
     */
    if (streamCallback_ != nullptr) {
        streamCallback_->OnCaptureStarted(captureId, info.streamIds_);
    }
    return V::NO_ERROR;
}

int32_t HybrisStreamOperatorVdiImpl::CancelCapture(int32_t captureId)
{
    CAMERA_VDI_LOGI("CancelCapture(%{public}s): captureId=%{public}d",
                    ohosCameraId_.c_str(), captureId);
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

} // namespace OHOS::Camera::Hybris
