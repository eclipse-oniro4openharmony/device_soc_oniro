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

// Jack state watcher
void StartJackWatcher(HybrisAudioAdapter *ad);
void StopJackWatcher(HybrisAudioAdapter *ad);

} // namespace

#endif
