/*
 * Copyright (c) 2026 Oniro Authors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef HYBRIS_AUDIO_INTERNAL_H
#define HYBRIS_AUDIO_INTERNAL_H

#include "hybris_audio_common.h"
#include <mutex>
#include <atomic>
#include <string>
#include <thread>

namespace OHOS::HDI::Audio::Hybris {

// VDI vtables are C structs that must be the first member so that
// (struct IAudioXxxVdi *) casts to/from the wrapper work.

struct HybrisAudioRender {
    struct IAudioRenderVdi vtable;    // MUST be first
    struct audio_stream_out *stream;
    HybrisAudioAdapter *owner;
    std::mutex lock;
    std::atomic<bool> started{false};
    std::atomic<bool> paused{false};
    struct AudioSampleAttributesVdi attrs;
    struct AudioDeviceDescriptorVdi desc;
    float volume = 1.0f;
    bool muted = false;
};

struct HybrisAudioCapture {
    struct IAudioCaptureVdi vtable;
    struct audio_stream_in *stream;
    HybrisAudioAdapter *owner;
    std::mutex lock;
    std::atomic<bool> started{false};
    struct AudioSampleAttributesVdi attrs;
    struct AudioDeviceDescriptorVdi desc;
    float volume = 1.0f;
    bool muted = false;
};

struct HybrisAudioAdapter {
    struct IAudioAdapterVdi vtable;
    struct audio_hw_device *hwdev;
    std::string adapterName;  // always "primary"
    std::mutex lock;
    // Last mode handed to the Android HAL.  It has no getter, and telling it
    // the mode it is already in restarts MTK's speech path mid-call, so the
    // only safe way to avoid redundant transitions is to remember them.
    audio_mode_t mode = AUDIO_MODE_NORMAL;
    // The live primary streams, if any.  Routing is a *stream* parameter in
    // Android's HAL contract (AUDIO_PARAMETER_STREAM_ROUTING, which
    // AudioFlinger sends through out->common.set_parameters); the device-level
    // set_parameters ignores it.  We have to keep the streams to hand to be
    // able to route at all — see SetRouting().
    HybrisAudioRender *outRender = nullptr;
    HybrisAudioCapture *inCapture = nullptr;
    // Jack-state worker state (plan A5)
    std::thread jackThread;
    std::atomic<bool> jackRun{false};
    struct IAudioCallbackVdi *paramObserver = nullptr;
    int8_t paramCookie = 0;
};

struct HybrisAudioManager {
    struct IAudioManagerVdi vtable;
    HybrisAudioAdapter *adapter = nullptr;  // single primary adapter
    std::mutex lock;
};

// Factory functions
void InitRenderVTable(struct IAudioRenderVdi *v);
void InitCaptureVTable(struct IAudioCaptureVdi *v);
void InitAdapterVTable(struct IAudioAdapterVdi *v);

// Audio scene -> Android mode + routing (plan §D6).  Called from the
// render/capture SelectScene hooks, which is where the framework delivers it.
void ApplyOutputScene(HybrisAudioAdapter *ad, const struct AudioSceneDescriptorVdi *scene);
void ApplyInputScene(HybrisAudioAdapter *ad, const struct AudioSceneDescriptorVdi *scene);

// Jack state watcher
void StartJackWatcher(HybrisAudioAdapter *ad);
void StopJackWatcher(HybrisAudioAdapter *ad);

} // namespace

#endif
