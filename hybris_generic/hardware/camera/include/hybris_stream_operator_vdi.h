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

#ifndef HYBRIS_STREAM_OPERATOR_VDI_H
#define HYBRIS_STREAM_OPERATOR_VDI_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "surface.h"
#include "v1_0/istream_operator_vdi.h"

#include "hybris_camera_metadata.h"
#include "hybris_ndk_api.h"

namespace OHOS {
namespace HybrisCamera {

using namespace OHOS::VDI::Camera::V1_0;

class HybrisCameraDevice;

/*
 * Maps the OHOS stream model onto a camera2 capture session.
 *
 * OHOS streams become AImageReaders (YUV_420_888 for PREVIEW/VIDEO, JPEG for
 * STILL_CAPTURE); CommitStreams builds the session from all of them at once,
 * which matches camera2's "configure everything, then stream" contract.
 * Capture(isStreaming=true) becomes a repeating request, Capture(false) a
 * single one — the same split camera2 already makes.
 *
 * Frames are copied on arrival into a SurfaceBuffer dequeued from the stream's
 * own producer (plan §D3): the Android image is a gralloc buffer belonging to
 * the reader, not something the OHOS consumer can be handed directly.
 */
class HybrisStreamOperator : public IStreamOperatorVdi {
public:
    HybrisStreamOperator(const NdkApi *ndk, HybrisCameraDevice *device,
                         const sptr<IStreamOperatorVdiCallback> &callback);
    ~HybrisStreamOperator() override;

    int32_t IsStreamsSupported(VdiOperationMode mode, const std::vector<uint8_t> &modeSetting,
                               const std::vector<VdiStreamInfo> &infos,
                               VdiStreamSupportType &type) override;
    int32_t CreateStreams(const std::vector<VdiStreamInfo> &streamInfos) override;
    int32_t ReleaseStreams(const std::vector<int32_t> &streamIds) override;
    int32_t CommitStreams(VdiOperationMode mode, const std::vector<uint8_t> &modeSetting) override;
    int32_t GetStreamAttributes(std::vector<VdiStreamAttribute> &attributes) override;
    int32_t AttachBufferQueue(int32_t streamId,
                              const sptr<BufferProducerSequenceable> &bufferProducer) override;
    int32_t DetachBufferQueue(int32_t streamId) override;
    int32_t Capture(int32_t captureId, const VdiCaptureInfo &info, bool isStreaming) override;
    int32_t CancelCapture(int32_t captureId) override;
    int32_t ChangeToOfflineStream(const std::vector<int32_t> &streamIds,
                                  const sptr<IStreamOperatorVdiCallback> &callbackObj,
                                  sptr<IOfflineStreamOperatorVdi> &offlineOperator) override;

    // Tears the capture session down; called when the device closes.
    void Shutdown();

private:
    // Per-reader listener context; must outlive the reader, so the stream owns
    // it and it is destroyed only after the reader has been deleted.
    struct ListenerCtx {
        HybrisStreamOperator *self;
        int32_t streamId;
    };

    struct Stream {
        VdiStreamInfo info {};
        sptr<OHOS::Surface> surface;
        AImageReader *reader = nullptr;
        ANativeWindow *window = nullptr;
        ACaptureSessionOutput *output = nullptr;
        ACameraOutputTarget *target = nullptr;
        std::unique_ptr<ListenerCtx> listenerCtx;
        bool isBlob = false;
        int32_t captureId = -1;
        bool streaming = false;
        bool shutterCallback = false;
        uint32_t frameCount = 0;
    };

    static void OnImageAvailable(void *context, AImageReader *reader);
    static void OnSessionClosed(void *context, ACameraCaptureSession *session);
    static void OnSessionReady(void *context, ACameraCaptureSession *session);
    static void OnSessionActive(void *context, ACameraCaptureSession *session);
    static void OnCaptureStartedCb(void *context, ACameraCaptureSession *session,
                                   const ACaptureRequest *request, int64_t timestamp);
    static void OnCaptureCompletedCb(void *context, ACameraCaptureSession *session,
                                     ACaptureRequest *request, const ACameraMetadata *result);
    static void OnCaptureFailedCb(void *context, ACameraCaptureSession *session,
                                  ACaptureRequest *request, ACameraCaptureFailure *failure);
    static void OnSequenceCompletedCb(void *context, ACameraCaptureSession *session, int sequenceId,
                                      int64_t frameNumber);
    static void OnSequenceAbortedCb(void *context, ACameraCaptureSession *session, int sequenceId);
    static void OnBufferLostCb(void *context, ACameraCaptureSession *session,
                               ACaptureRequest *request, ACameraWindowType *window,
                               int64_t frameNumber);

    // Everything Capture() does under the lock; the framework callback it owes
    // is sent by the caller once the lock is gone.
    int32_t SubmitCapture(int32_t captureId, const VdiCaptureInfo &info, bool isStreaming,
                          std::vector<int32_t> &targets);
    void HandleImage(int32_t streamId, AImageReader *reader);
    bool DeliverYuv(Stream &stream, AImage *image, int64_t timestamp);
    bool DeliverBlob(Stream &stream, AImage *image, int64_t timestamp);
    std::shared_ptr<Stream> FindStream(int32_t streamId);

    /*
     * Destroys the Android objects behind a stream, and closes a session.
     *
     * Both MUST be called with lock_ released: AImageReader_delete waits for an
     * in-flight image callback to return, ACameraCaptureSession_close and
     * stopRepeating wait for capture callbacks, and those callbacks take lock_.
     * Holding it across either is the same deadlock the composer VDI hit when
     * it fired hotplug callbacks under its own mutex.
     */
    void DestroyStream(const std::shared_ptr<Stream> &stream);
    void CloseSession(ACameraCaptureSession *session, ACaptureRequest *repeating,
                      ACaptureSessionOutputContainer *outputs);

    const NdkApi *ndk_;
    HybrisCameraDevice *device_;
    sptr<IStreamOperatorVdiCallback> callback_;

    std::mutex lock_;
    std::map<int32_t, std::shared_ptr<Stream>> streams_;
    ACaptureSessionOutputContainer *outputs_ = nullptr;
    ACameraCaptureSession *session_ = nullptr;
    ACaptureRequest *repeatingRequest_ = nullptr;
    bool committed_ = false;
};

} // namespace HybrisCamera
} // namespace OHOS

#endif // HYBRIS_STREAM_OPERATOR_VDI_H
