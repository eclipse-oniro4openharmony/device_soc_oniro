/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Hybris Stream Operator VDI — implements IStreamOperatorVdi.
 *
 * Bring-up scope (N12.6.0 — instrumentation only):
 *   - Accepts CreateStreams / CommitStreams / AttachBufferQueue and
 *     logs the requested configuration so we can see what the OHOS
 *     framework expects.
 *   - Capture / CancelCapture / ChangeToOfflineStream return
 *     METHOD_NOT_SUPPORTED — the Halium ICameraDeviceSession bridge
 *     (open + configureStreams_3_4 + processCaptureRequest) lands in
 *     N12.6.1+.
 *
 * The stub lets the OHOS camera HAP progress past
 * HCameraDevice::GetStreamOperator (currently fails with code=-4) so
 * we can observe the framework's CreateStreams shape on real hardware
 * before committing to a configureStreams encoder.
 */

#ifndef HYBRIS_STREAM_OPERATOR_VDI_IMPL_H
#define HYBRIS_STREAM_OPERATOR_VDI_IMPL_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "v1_0/istream_operator_vdi.h"
#include "v1_0/istream_operator_vdi_callback.h"
#include "v1_0/vdi_types.h"

namespace OHOS::Camera::Hybris {

namespace Hidl {
class HwBinderClient;
class HwCameraDevice;
} // namespace Hidl

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
    HybrisStreamOperatorVdiImpl(Hidl::HwBinderClient *client,
                                Hidl::HwCameraDevice *device,
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
    std::mutex                              mutex_;
    [[maybe_unused]] Hidl::HwBinderClient  *client_;          /* borrowed */
    [[maybe_unused]] Hidl::HwCameraDevice  *device_;          /* borrowed */
    std::string                             ohosCameraId_;
    sptr<IStreamOperatorVdiCallback>        streamCallback_;

    struct StreamRecord {
        VdiStreamInfo                       info;
        sptr<BufferProducerSequenceable>    producer;
    };
    std::map<int32_t, StreamRecord>         streams_;
};

} // namespace OHOS::Camera::Hybris

#endif // HYBRIS_STREAM_OPERATOR_VDI_IMPL_H
