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

/*
 * hybris_camera_hdi_test — exercises the whole OHOS camera stack (SA 3008 ->
 * camera_host -> our VDI -> the container's CameraService) from a native
 * client, using the camera_framework inner API.
 *
 * The stock com.ohos.camera app cannot serve as the bring-up client on this
 * port: its onPageShow calls getPhotoAccessHelper, and the medialibrary
 * DataShareExtAbility never finishes connecting, so the app is ANR-killed
 * before a viewfinder ever appears.  That is a media-library gap, not a camera
 * one — this harness proves the camera path without it.
 *
 *   hybris_camera_hdi_test --list
 *   hybris_camera_hdi_test --preview 60 --photo /data/hdi.jpg
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include "accesstoken_kit.h"
#include "nativetoken_kit.h"
#include "token_setproc.h"

#include "input/camera_manager.h"
#include "output/photo_output.h"
#include "output/preview_output.h"
#include "session/capture_session.h"
#include "surface.h"
#include "video_key_info.h"

using namespace OHOS;
using namespace OHOS::CameraStandard;

namespace {

double NowMs()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

double g_t0 = 0;

void Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[%8.1f] ", NowMs() - g_t0);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

} // namespace

// Drains a consumer surface, counting frames and optionally dumping the first
// one so the pixel content can be eyeballed off-device.  Deliberately not in
// an anonymous namespace: graphic_surface calls OnBufferAvailable across the
// DSO boundary and a local vtable upsets cross-DSO CFI.
class FrameSink : public IBufferConsumerListener {
public:
    FrameSink(sptr<Surface> surface, std::string dumpPath, bool isPhoto)
        : surface_(surface), dumpPath_(std::move(dumpPath)), isPhoto_(isPhoto)
    {
    }

    void OnBufferAvailable() override
    {
        sptr<SurfaceBuffer> buffer = nullptr;
        int32_t fence = -1;
        int64_t timestamp = 0;
        OHOS::Rect damage {};
        if (surface_->AcquireBuffer(buffer, fence, timestamp, damage) != SURFACE_ERROR_OK ||
            buffer == nullptr) {
            return;
        }
        int32_t size = static_cast<int32_t>(buffer->GetSize());
        if (isPhoto_ && buffer->GetExtraData() != nullptr) {
            int32_t dataSize = 0;
            if (buffer->GetExtraData()->ExtraGet(OHOS::Camera::dataSize, dataSize) == 0 &&
                dataSize > 0) {
                size = dataSize;
            }
        }
        uint32_t seen = ++count_;
        if (seen == 1) {
            Log("%s: first buffer %dx%d stride=%d size=%d ts=%lld", isPhoto_ ? "photo" : "preview",
                buffer->GetWidth(), buffer->GetHeight(), buffer->GetStride(), size,
                static_cast<long long>(timestamp));
            if (!dumpPath_.empty() && buffer->GetVirAddr() != nullptr) {
                FILE *f = fopen(dumpPath_.c_str(), "wb");
                if (f != nullptr) {
                    size_t wrote = fwrite(buffer->GetVirAddr(), 1, static_cast<size_t>(size), f);
                    fclose(f);
                    Log("%s: wrote %zu bytes to %s", isPhoto_ ? "photo" : "preview", wrote,
                        dumpPath_.c_str());
                }
            }
        }
        surface_->ReleaseBuffer(buffer, -1);
        cv_.notify_all();
    }

    uint32_t Count() const { return count_.load(); }

    bool WaitFor(uint32_t target, int seconds)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(seconds),
                            [this, target] { return count_.load() >= target; });
    }

private:
    sptr<Surface> surface_;
    std::string dumpPath_;
    bool isPhoto_;
    std::atomic<uint32_t> count_ { 0 };
    std::mutex mutex_;
    std::condition_variable cv_;
};

namespace {

void DumpProfiles(const char *label, std::vector<Profile> profiles)
{
    printf("    %s (%zu):", label, profiles.size());
    int shown = 0;
    for (auto &p : profiles) {
        printf(" %ux%u/f%d", p.GetSize().width, p.GetSize().height, static_cast<int>(p.GetCameraFormat()));
        if (++shown >= 12) {
            printf(" …");
            break;
        }
    }
    printf("\n");
    fflush(stdout);
}

// A native binary carries no access token, so every CameraManager entry point
// that checks ohos.permission.CAMERA refuses it.  Mint a system_basic native
// token for ourselves, the way the in-tree camera/media tests do.
void GrantSelfCameraPermission()
{
    const char *perms[] = {
        "ohos.permission.CAMERA",
        "ohos.permission.MICROPHONE",
        "ohos.permission.WRITE_MEDIA",
        "ohos.permission.READ_MEDIA",
    };
    NativeTokenInfoParams params = {
        .dcapsNum = 0,
        .permsNum = sizeof(perms) / sizeof(perms[0]),
        .aclsNum = 0,
        .dcaps = nullptr,
        .perms = perms,
        .acls = nullptr,
        .processName = "hybris_camera_hdi_test",
        .aplStr = "system_basic",
    };
    uint64_t tokenId = GetAccessTokenId(&params);
    if (tokenId == 0) {
        Log("GetAccessTokenId failed — camera calls will be denied");
        return;
    }
    int ret = SetSelfTokenID(tokenId);
    int reload = OHOS::Security::AccessToken::AccessTokenKit::ReloadNativeTokenInfo();
    Log("granted self camera permission: token=%llu setSelf=%d reload=%d",
        static_cast<unsigned long long>(tokenId), ret, reload);
}

} // namespace

int main(int argc, char **argv)
{
    g_t0 = NowMs();
    bool listOnly = false;
    uint32_t previewFrames = 60;
    std::string photoPath = "/data/hdi.jpg";
    std::string previewPath;
    int cameraIndex = 0;
    int soakSec = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list") {
            listOnly = true;
        } else if (a == "--preview" && i + 1 < argc) {
            previewFrames = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        } else if (a == "--photo" && i + 1 < argc) {
            photoPath = argv[++i];
        } else if (a == "--no-photo") {
            photoPath.clear();
        } else if (a == "--dump-preview" && i + 1 < argc) {
            previewPath = argv[++i];
        } else if (a == "--camera" && i + 1 < argc) {
            cameraIndex = static_cast<int>(strtol(argv[++i], nullptr, 0));
        } else if (a == "--soak" && i + 1 < argc) {
            soakSec = static_cast<int>(strtol(argv[++i], nullptr, 0));
        } else {
            printf("usage: %s [--list] [--camera N] [--preview N] [--photo PATH] "
                   "[--dump-preview PATH] [--no-photo] [--soak SEC]\n", argv[0]);
            return 1;
        }
    }

    GrantSelfCameraPermission();

    sptr<CameraManager> manager = CameraManager::GetInstance();
    if (manager == nullptr) {
        Log("CameraManager::GetInstance returned null");
        return 2;
    }
    std::vector<sptr<CameraDevice>> cameras = manager->GetSupportedCameras();
    Log("GetSupportedCameras -> %zu", cameras.size());
    if (cameras.empty()) {
        Log("*** no cameras: the HDI has not published any ***");
        return 3;
    }
    for (size_t i = 0; i < cameras.size(); ++i) {
        Log("  camera[%zu] id=%s position=%d type=%d connection=%d orientation=%d", i,
            cameras[i]->GetID().c_str(), static_cast<int>(cameras[i]->GetPosition()),
            static_cast<int>(cameras[i]->GetCameraType()),
            static_cast<int>(cameras[i]->GetConnectionType()),
            cameras[i]->GetCameraOrientation());
        auto cap = manager->GetSupportedOutputCapability(cameras[i], SceneMode::CAPTURE);
        if (cap != nullptr) {
            DumpProfiles("preview", cap->GetPreviewProfiles());
            DumpProfiles("photo", cap->GetPhotoProfiles());
        }
    }
    if (listOnly) {
        return 0;
    }
    if (cameraIndex < 0 || cameraIndex >= static_cast<int>(cameras.size())) {
        Log("camera index %d out of range", cameraIndex);
        return 4;
    }

    sptr<CameraDevice> camera = cameras[cameraIndex];
    auto capability = manager->GetSupportedOutputCapability(camera, SceneMode::CAPTURE);
    if (capability == nullptr || capability->GetPreviewProfiles().empty() ||
        capability->GetPhotoProfiles().empty()) {
        Log("*** camera %s has no usable profiles ***", camera->GetID().c_str());
        return 5;
    }

    // Prefer a 1280x720 preview so the copy path is exercised at a realistic
    // size rather than at whatever happens to be first.
    Profile previewProfile = capability->GetPreviewProfiles()[0];
    for (auto p : capability->GetPreviewProfiles()) {
        if (p.GetSize().width == 1280 && p.GetSize().height == 720) {
            previewProfile = p;
            break;
        }
    }
    Profile photoProfile = capability->GetPhotoProfiles()[0];
    for (auto p : capability->GetPhotoProfiles()) {
        if (p.GetSize().width == 1920 && p.GetSize().height == 1080) {
            photoProfile = p;
            break;
        }
    }
    Log("using preview %ux%u fmt=%d, photo %ux%u fmt=%d", previewProfile.GetSize().width,
        previewProfile.GetSize().height, static_cast<int>(previewProfile.GetCameraFormat()),
        photoProfile.GetSize().width, photoProfile.GetSize().height,
        static_cast<int>(photoProfile.GetCameraFormat()));

    sptr<CameraInput> input = manager->CreateCameraInput(camera);
    if (input == nullptr) {
        Log("CreateCameraInput failed");
        return 6;
    }
    int32_t ret = input->Open();
    Log("CameraInput::Open -> %d", ret);
    if (ret != 0) {
        return 7;
    }

    sptr<Surface> previewSurface = Surface::CreateSurfaceAsConsumer();
    sptr<FrameSink> previewSink = new FrameSink(previewSurface, previewPath, false);
    sptr<IBufferConsumerListener> previewListener = previewSink;
    previewSurface->RegisterConsumerListener(previewListener);
    sptr<PreviewOutput> preview = manager->CreatePreviewOutput(previewProfile, previewSurface);
    if (preview == nullptr) {
        Log("CreatePreviewOutput failed");
        return 8;
    }

    sptr<Surface> photoSurface = Surface::CreateSurfaceAsConsumer();
    sptr<FrameSink> photoSink = new FrameSink(photoSurface, photoPath, true);
    sptr<IBufferConsumerListener> photoListener = photoSink;
    photoSurface->RegisterConsumerListener(photoListener);
    sptr<IBufferProducer> photoProducer = photoSurface->GetProducer();
    sptr<PhotoOutput> photo = manager->CreatePhotoOutput(photoProfile, photoProducer);
    if (photo == nullptr) {
        Log("CreatePhotoOutput failed");
        return 9;
    }

    sptr<CaptureSession> session = manager->CreateCaptureSession();
    if (session == nullptr) {
        Log("CreateCaptureSession failed");
        return 10;
    }
    sptr<CaptureInput> captureInput = input;
    sptr<CaptureOutput> previewOutput = preview;
    sptr<CaptureOutput> photoOutput = photo;

    Log("BeginConfig -> %d", session->BeginConfig());
    Log("AddInput -> %d", session->AddInput(captureInput));
    Log("AddOutput(preview) -> %d", session->AddOutput(previewOutput));
    if (!photoPath.empty()) {
        Log("AddOutput(photo) -> %d", session->AddOutput(photoOutput));
    }
    ret = session->CommitConfig();
    Log("CommitConfig -> %d", ret);
    if (ret != 0) {
        session->Release();
        input->Close();
        return 11;
    }

    double t = NowMs();
    ret = session->Start();
    Log("Session::Start -> %d", ret);

    bool ok = previewSink->WaitFor(previewFrames, 20);
    double span = NowMs() - t;
    Log("preview: %u frames in %.0f ms => %.1f fps (target %u, timeout=%d)", previewSink->Count(),
        span, previewSink->Count() * 1000.0 / span, previewFrames, ok ? 0 : 1);

    if (!photoPath.empty()) {
        t = NowMs();
        ret = photo->Capture();
        Log("PhotoOutput::Capture -> %d", ret);
        bool got = photoSink->WaitFor(1, 20);
        Log("photo %s after %.0f ms", got ? "ARRIVED" : "TIMED OUT", NowMs() - t);
    }

    if (soakSec > 0) {
        uint32_t before = previewSink->Count();
        double t1 = NowMs();
        sleep(static_cast<unsigned>(soakSec));
        double s = NowMs() - t1;
        Log("soak: %u frames in %.0f ms => %.1f fps", previewSink->Count() - before, s,
            (previewSink->Count() - before) * 1000.0 / s);
    }

    Log("Session::Stop -> %d", session->Stop());
    Log("Session::Release -> %d", session->Release());
    Log("Input::Close -> %d", input->Close());
    Log("done: %u preview frames, %u photo buffers", previewSink->Count(), photoSink->Count());
    return 0;
}
