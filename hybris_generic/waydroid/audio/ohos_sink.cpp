/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * See ohos_sink.h.
 */

#include "ohos_sink.h"

#include <algorithm>
#include <chrono>

#include "audio_info.h"
#include "audio_renderer.h"
#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "waydroid_audio"

using namespace OHOS::AudioStandard;

namespace waydroid {
namespace {

/*
 * How much we hand the renderer per write.  A small write keeps the
 * latency honest; too small and we pay a binder round trip per 2 ms.
 * 20 ms is what the container's AudioFlinger mixes in anyway.
 */
constexpr uint64_t WRITE_CHUNK_USEC = 20000;
constexpr size_t MIN_WRITE_CHUNK = 1024;

AudioSampleFormat ToOhosFormat(uint8_t paFormat, bool &supported)
{
    supported = true;
    switch (paFormat) {
        case pa::SAMPLE_U8: return SAMPLE_U8;
        case pa::SAMPLE_S16LE: return SAMPLE_S16LE;
        case pa::SAMPLE_S24LE: return SAMPLE_S24LE;
        case pa::SAMPLE_S32LE: return SAMPLE_S32LE;
        case pa::SAMPLE_FLOAT32LE: return SAMPLE_F32LE;
        default:
            supported = false;
            return SAMPLE_S16LE;
    }
}

} // namespace

OhosSink::~OhosSink()
{
    Close();
}

bool OhosSink::Open(const pa::SampleSpec &ss, size_t ringBytes, std::function<void()> notify)
{
    bool supported = false;
    AudioSampleFormat format = ToOhosFormat(ss.format, supported);
    if (!supported) {
        HILOG_ERROR(LOG_CORE, "unsupported PA sample format %{public}u", ss.format);
        return false;
    }
    if (ss.channels < 1 || ss.channels > 2) {
        /*
         * The container's primary output is stereo.  Anything else would
         * need a channel layout we have no way to guess, so refuse it and
         * let the HAL renegotiate rather than play noise.
         */
        HILOG_ERROR(LOG_CORE, "unsupported channel count %{public}u", ss.channels);
        return false;
    }

    AudioRendererOptions options;
    options.streamInfo.samplingRate = static_cast<AudioSamplingRate>(ss.rate);
    options.streamInfo.encoding = ENCODING_PCM;
    options.streamInfo.format = format;
    options.streamInfo.channels = static_cast<AudioChannel>(ss.channels);
    options.rendererInfo.contentType = CONTENT_TYPE_MUSIC;
    options.rendererInfo.streamUsage = STREAM_USAGE_MEDIA;
    options.rendererInfo.rendererFlags = AUDIO_FLAG_NORMAL;

    std::unique_ptr<AudioRenderer> renderer = AudioRenderer::Create(options);
    if (renderer == nullptr) {
        HILOG_ERROR(LOG_CORE, "AudioRenderer::Create failed (%{public}u Hz, %{public}u ch)",
                     ss.rate, ss.channels);
        return false;
    }
    /*
     * Deliberately not Start()ed here.  A stream is usually created
     * corked, and a running renderer with nothing to write underruns
     * (and logs) forever; the render thread starts it on the first real
     * chunk and pauses it again when the stream is corked.
     */
    renderer_ = std::move(renderer);
    spec_ = ss;
    ring_.assign(ringBytes, 0);
    head_ = 0;
    fill_ = 0;
    readIndex_ = 0;
    corked_ = true;
    quit_ = false;
    playing_ = false;
    notify_ = std::move(notify);
    writeFailed_ = false;

    thread_ = std::thread([this] { RenderLoop(); });
    HILOG_INFO(LOG_CORE, "sink open: %{public}u Hz %{public}u ch, ring %{public}zu B",
                ss.rate, ss.channels, ringBytes);
    return true;
}

void OhosSink::Close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quit_) {
            return;
        }
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (renderer_ != nullptr) {
        renderer_->Stop();
        renderer_->Release();
        renderer_.reset();
    }
}

size_t OhosSink::Push(const uint8_t *data, size_t len)
{
    size_t accepted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quit_ || ring_.empty()) {
            return 0;
        }
        accepted = std::min(len, ring_.size() - fill_);
        size_t tail = (head_ + fill_) % ring_.size();
        size_t first = std::min(accepted, ring_.size() - tail);
        std::copy(data, data + first, ring_.begin() + tail);
        if (accepted > first) {
            std::copy(data + first, data + accepted, ring_.begin());
        }
        fill_ += accepted;
    }
    cv_.notify_one();
    return accepted;
}

size_t OhosSink::Buffered()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return fill_;
}

void OhosSink::Flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    fill_ = 0;
    playing_ = false;
    drainRequested_ = false;
}

void OhosSink::SetCorked(bool corked)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        corked_ = corked;
    }
    cv_.notify_one();
}

void OhosSink::RequestDrain()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drainRequested_ = true;
    }
    cv_.notify_one();
}

OhosSink::Events OhosSink::TakeEvents()
{
    std::lock_guard<std::mutex> lock(mutex_);
    Events out = pending_;
    pending_ = Events {};
    return out;
}

uint64_t OhosSink::ReadIndex()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return readIndex_;
}

bool OhosSink::IsPlaying()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_ && !corked_;
}

uint64_t OhosSink::SinkLatencyUsec()
{
    if (renderer_ == nullptr) {
        return 0;
    }
    uint64_t latency = 0;
    if (renderer_->GetLatency(latency) != 0) {
        return 0;
    }
    return latency;
}

size_t OhosSink::PopLocked(uint8_t *dst, size_t want)
{
    size_t got = std::min(want, fill_);
    size_t first = std::min(got, ring_.size() - head_);
    std::copy(ring_.begin() + head_, ring_.begin() + head_ + first, dst);
    if (got > first) {
        std::copy(ring_.begin(), ring_.begin() + (got - first), dst + first);
    }
    head_ = (head_ + got) % ring_.size();
    fill_ -= got;
    return got;
}

void OhosSink::RenderLoop()
{
    size_t chunk = static_cast<size_t>(spec_.UsecToBytes(WRITE_CHUNK_USEC));
    chunk = std::max(chunk, MIN_WRITE_CHUNK);
    chunk = std::min(chunk, ring_.size());
    std::vector<uint8_t> scratch(chunk);

    for (;;) {
        size_t got = 0;
        bool finishDrain = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return quit_ || (!corked_ && fill_ > 0) || drainRequested_;
            });
            if (quit_) {
                return;
            }
            if (!corked_ && fill_ > 0) {
                got = PopLocked(scratch.data(), scratch.size());
            } else if (drainRequested_ && fill_ == 0) {
                drainRequested_ = false;
                finishDrain = true;
            } else if (corked_ && rendererRunning_) {
                lock.unlock();
                renderer_->Pause();
                rendererRunning_ = false;
                continue;
            } else if (!corked_ && fill_ == 0 && playing_) {
                /*
                 * Ran dry while uncorked: that is an underrun in PA's
                 * terms.  Report it once, then stay quiet until audio
                 * flows again, or a paused stream would spam the client.
                 */
                playing_ = false;
                pending_.underflow = true;
                lock.unlock();
                if (notify_) {
                    notify_();
                }
                continue;
            }
        }

        if (finishDrain) {
            if (renderer_ != nullptr) {
                renderer_->Drain();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_.drained = true;
            }
            if (notify_) {
                notify_();
            }
            continue;
        }

        if (got == 0) {
            continue;
        }

        /*
         * The blocking write is the clock.  A short write means the
         * renderer took only part of it, so loop until the chunk is gone.
         */
        if (!rendererRunning_) {
            rendererRunning_ = renderer_->Start();
            if (!rendererRunning_ && !startFailed_.exchange(true)) {
                HILOG_ERROR(LOG_CORE, "AudioRenderer::Start failed");
            }
        }

        size_t written = 0;
        while (rendererRunning_ && written < got) {
            int32_t n = renderer_->Write(scratch.data() + written, got - written);
            if (n <= 0) {
                if (!writeFailed_.exchange(true)) {
                    HILOG_ERROR(LOG_CORE, "AudioRenderer::Write failed: %{public}d", n);
                }
                break;
            }
            written += static_cast<size_t>(n);
        }

        if (written < got) {
            /*
             * The blocking write is our only clock.  When it is not
             * happening — the renderer would not start, or it failed —
             * nothing paces this loop, and it would spin through the ring
             * and pull the container's audio clock far ahead of real time:
             * exactly the failure this whole daemon exists to fix.  So
             * burn the wall-clock time the dropped bytes were worth.
             */
            std::this_thread::sleep_for(std::chrono::microseconds(spec_.BytesToUsec(got - written)));
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            readIndex_ += got;
            pending_.credit += got;
            if (!playing_) {
                playing_ = true;
                pending_.started = true;
            }
        }
        if (notify_) {
            notify_();
        }
    }
}

} // namespace waydroid
