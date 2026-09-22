/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ohos_sink.h — where the container's audio actually goes.
 *
 * One PulseAudio playback stream maps to one OHOS AudioRenderer, so the
 * container's streams arrive in OHOS audio policy as ordinary media
 * streams: they mix with OHOS sound, follow the media volume key and
 * follow routing (speaker, headset, Bluetooth) for free.  Mixing several
 * of them is the policy's job, not ours.
 *
 * The sink owns the pacing.  AudioRenderer::Write() blocks until the
 * stream has room, which is exactly the back-pressure a real PCM device
 * would give the container: a render thread drains the ring at the rate
 * the hardware consumes it, and every drained byte becomes credit we
 * hand back to the client as a PA REQUEST.  That is the whole reason the
 * container's AudioFlinger will run at wall-clock speed again.
 */

#ifndef WAYDROID_OHOS_SINK_H
#define WAYDROID_OHOS_SINK_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pa_wire.h"

namespace OHOS {
namespace AudioStandard {
class AudioRenderer;
}
} // namespace OHOS

namespace waydroid {

class OhosSink {
public:
    OhosSink() = default;
    ~OhosSink();

    OhosSink(const OhosSink &) = delete;
    OhosSink &operator=(const OhosSink &) = delete;

    /*
     * notify is called from the render thread whenever the protocol loop
     * has something to do (credit to hand back, an underflow to report, a
     * drain that finished).  It must be safe to call from another thread —
     * in practice it writes a byte to the loop's eventfd.
     */
    bool Open(const pa::SampleSpec &ss, size_t ringBytes, std::function<void()> notify);
    void Close();

    /* Producer side, called from the protocol loop. */
    size_t Push(const uint8_t *data, size_t len);
    size_t Buffered();
    void Flush();
    void SetCorked(bool corked);
    void RequestDrain();

    /*
     * Events collected for the protocol loop.  Each call clears what it
     * returns, so a wake-up never reports the same thing twice.
     */
    struct Events {
        uint64_t credit = 0;   /* bytes drained since the last poll */
        bool underflow = false;
        bool started = false;
        bool drained = false;
    };
    Events TakeEvents();

    /* Total bytes handed to the renderer; PA's "read index". */
    uint64_t ReadIndex();
    /* The renderer's own latency, for the sink_usec field of a latency reply. */
    uint64_t SinkLatencyUsec();
    bool IsPlaying();

private:
    void RenderLoop();
    size_t PopLocked(uint8_t *dst, size_t want);

    std::shared_ptr<OHOS::AudioStandard::AudioRenderer> renderer_;
    pa::SampleSpec spec_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<uint8_t> ring_;
    size_t head_ = 0;   /* read cursor */
    size_t fill_ = 0;   /* bytes currently buffered */
    bool corked_ = true;
    bool quit_ = false;
    bool drainRequested_ = false;
    /* True once we have written audio and not yet reported running dry. */
    bool playing_ = false;

    Events pending_;
    uint64_t readIndex_ = 0;

    /* Render-thread only: whether the renderer is in RUNNING state. */
    bool rendererRunning_ = false;

    std::function<void()> notify_;
    std::thread thread_;
    std::atomic<bool> writeFailed_ { false };
    std::atomic<bool> startFailed_ { false };
};

} // namespace waydroid

#endif /* WAYDROID_OHOS_SINK_H */
