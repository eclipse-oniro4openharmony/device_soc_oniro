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
 * libhevcdec_ohos.z.so: av_codec's H.265 decoder, on the phone's hardware.
 *
 * av_codec's HevcDecoder (foundation/multimedia/av_codec/services/engine/
 * codec/video/hevcdecoder) dlopens this library and only offers video/hevc
 * decoding when it exists; upstream expects a closed vendor blob, so HEVC
 * videos did not play at all.  This one drives the Android side's MediaCodec
 * decoder (MediaTek's Codec2 c2.mtk.hevc.decoder on the Volla phones)
 * through the NDK's AMediaCodec, loaded with libhybris (hybris_mediandk.cpp).
 *
 * The contract, read from hevc_decoder.cpp:
 *
 *   INT32 HEVC_CreateDecoder(HEVC_DEC_HANDLE *, HEVC_DEC_INIT_PARAM *);
 *   INT32 HEVC_DecodeFrame(HEVC_DEC_HANDLE, HEVC_DEC_INARGS *, HEVC_DEC_OUTARGS *);
 *   INT32 HEVC_FlushFrame(HEVC_DEC_HANDLE, HEVC_DEC_OUTARGS *);
 *   INT32 HEVC_DeleteDecoder(HEVC_DEC_HANDLE);
 *
 *  - DecodeFrame gets one access unit (Annex B, parameter sets in band) and
 *    reports how many bytes it consumed; av_codec calls it again while bytes
 *    are left.  It returns 0 with a picture in the out args, a positive value
 *    for "consumed, no picture yet", and anything below -1 on error.
 *  - FlushFrame is called at end of stream (and before a seek) until it
 *    stops returning 0, to drain the pictures still in the decoder.
 *  - Pictures are planar YUV 4:2:0 in pucOutYUV[0..2] with uiDecStride; the
 *    memory only has to live until the next call.
 *
 * A hardware decoder returns pictures some frames after their input (it
 * reorders, and its pipeline is deep), so each input is queued whole and the
 * call hands back the oldest finished picture if there is one; pictures that
 * finish early wait in a queue for the following calls, and FlushFrame
 * drains the rest.  Output is always 8-bit: Main10 pictures (P010) are
 * reduced to 8 bits, since av_codec's 10-bit path expects a layout this
 * decoder does not produce.
 */

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

#include "hilog/log.h"

#include "HevcDec_Typedef.h"
#include "mediandk_abi.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002562
#define LOG_TAG "OniroHevcDec"
#define HD_LOGI(fmt, ...) HILOG_INFO(LOG_CORE, fmt, ##__VA_ARGS__)
#define HD_LOGW(fmt, ...) HILOG_WARN(LOG_CORE, fmt, ##__VA_ARGS__)
#define HD_LOGE(fmt, ...) HILOG_ERROR(LOG_CORE, fmt, ##__VA_ARGS__)

#define HEVCDEC_EXPORT extern "C" __attribute__((visibility("default")))

namespace OHOS {
namespace OniroCodec {
namespace {

constexpr INT32 RET_PICTURE = 0;
constexpr INT32 RET_NO_PICTURE = 1;  /* input consumed, nothing to show yet */
constexpr INT32 RET_DRAINED = -1;    /* FlushFrame: nothing left */
constexpr INT32 RET_ERROR = -2;

constexpr const char *MIME_HEVC = "video/hevc";
constexpr const char *PREFERRED_CODEC = "c2.mtk.hevc.decoder";

/* Colour formats MediaCodec can hand back for byte-buffer output. */
constexpr int32_t COLOR_YUV420_PLANAR = 19;
constexpr int32_t COLOR_YUV420_SEMIPLANAR = 21;
constexpr int32_t COLOR_YUV420_FLEXIBLE = 0x7F420888;
constexpr int32_t COLOR_YUVP010 = 54;

/* Configure-time size; the decoder follows the stream's real one. */
constexpr int32_t DEFAULT_WIDTH = 1920;
constexpr int32_t DEFAULT_HEIGHT = 1080;
constexpr int32_t MAX_WIDTH = 4096;
constexpr int32_t MAX_HEIGHT = 2304;

constexpr int64_t INPUT_WAIT_US = 20 * 1000;
constexpr int INPUT_TRIES = 50;               /* ~1 s for a free input buffer */
constexpr int64_t AFTER_QUEUE_WAIT_US = 8 * 1000;
constexpr int64_t DRAIN_WAIT_US = 40 * 1000;
constexpr auto DRAIN_LIMIT = std::chrono::milliseconds(2000);

using Clock = std::chrono::steady_clock;

struct Picture {
    std::vector<uint8_t> yuv; /* tight I420 */
    int32_t width = 0;
    int32_t height = 0;
    int64_t pts = 0;
};

struct OutLayout {
    int32_t color = COLOR_YUV420_PLANAR;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    int32_t slice = 0;
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = -1;  /* inclusive, as MediaFormat's crop */
    int32_t bottom = -1;
};

class Session {
public:
    explicit Session(const MediaNdk *ndk) : ndk_(ndk) {}

    ~Session()
    {
        if (codec_ != nullptr) {
            (void)ndk_->stop(codec_);
            (void)ndk_->destroy(codec_);
        }
    }

    bool Open()
    {
        codec_ = ndk_->createCodecByName(PREFERRED_CODEC);
        if (codec_ == nullptr) {
            HD_LOGW("%{public}s unavailable, asking MediaCodecList for any %{public}s decoder", PREFERRED_CODEC,
                    MIME_HEVC);
            codec_ = ndk_->createDecoderByType(MIME_HEVC);
        }
        if (codec_ == nullptr) {
            HD_LOGE("no %{public}s decoder", MIME_HEVC);
            return false;
        }
        AMediaFormat *fmt = ndk_->formatNew();
        ndk_->formatSetString(fmt, "mime", MIME_HEVC);
        ndk_->formatSetInt32(fmt, "width", DEFAULT_WIDTH);
        ndk_->formatSetInt32(fmt, "height", DEFAULT_HEIGHT);
        ndk_->formatSetInt32(fmt, "max-width", MAX_WIDTH);
        ndk_->formatSetInt32(fmt, "max-height", MAX_HEIGHT);
        ndk_->formatSetInt32(fmt, "color-format", COLOR_YUV420_PLANAR);
        media_status_t st = ndk_->configure(codec_, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_DECODE);
        (void)ndk_->formatDelete(fmt);
        if (st != AMEDIA_OK) {
            HD_LOGE("configure failed: %{public}d", st);
            return false;
        }
        st = ndk_->start(codec_);
        if (st != AMEDIA_OK) {
            HD_LOGE("start failed: %{public}d", st);
            return false;
        }
        return true;
    }

    INT32 Decode(const HEVC_DEC_INARGS *in, HEVC_DEC_OUTARGS *out)
    {
        out->uiBytsConsumed = 0;
        const bool haveInput = in != nullptr && in->pStream != nullptr && in->uiStreamLen > 0;
        /*
         * av_codec takes at most one picture per call and calls again only
         * while bytes are left.  When more pictures are waiting than this
         * sample can carry out, report nothing consumed: the same sample
         * comes back (recognised, not queued twice) and collects the next
         * picture, so a backlog never builds up.
         */
        const bool requeue = haveInput && in->pStream == pendingData_ && in->uiStreamLen == pendingLen_;
        if (haveInput && !requeue) {
            if (!Rearm() || !Queue(in->pStream, in->uiStreamLen, static_cast<int64_t>(in->uiTimeStamp), 0)) {
                return RET_ERROR;
            }
            pendingData_ = in->pStream;
            pendingLen_ = in->uiStreamLen;
            Drain(AFTER_QUEUE_WAIT_US);
        } else {
            Drain(0);
        }
        const bool picture = Hand(out);
        if (haveInput && (!picture || ready_.empty())) {
            out->uiBytsConsumed = in->uiStreamLen;
            pendingData_ = nullptr;
            pendingLen_ = 0;
        }
        return picture ? RET_PICTURE : RET_NO_PICTURE;
    }

    INT32 Flush(HEVC_DEC_OUTARGS *out)
    {
        if (!eosQueued_ && !eosSeen_) {
            if (queuedSinceRearm_ == 0) {
                eosSeen_ = true; /* nothing was ever fed: nothing to drain */
            } else if (Queue(nullptr, 0, lastPts_, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) {
                eosQueued_ = true;
            } else {
                eosSeen_ = true;
            }
        }
        const auto deadline = Clock::now() + DRAIN_LIMIT;
        while (ready_.empty() && !eosSeen_ && Clock::now() < deadline) {
            Drain(DRAIN_WAIT_US);
        }
        if (Hand(out)) {
            return RET_PICTURE;
        }
        if (!eosSeen_) {
            HD_LOGW("drain timed out with %{public}d picture(s) unaccounted", queuedSinceRearm_);
            eosSeen_ = true;
        }
        return RET_DRAINED;
    }

    uint64_t Pictures() const
    {
        return pictures_;
    }

private:
    /* After a drain the codec sits at end of stream; a new input (playback
     * after a seek) needs it flushed back to the running state. */
    bool Rearm()
    {
        if (!eosQueued_ && !eosSeen_) {
            return true;
        }
        if (ndk_->flush(codec_) != AMEDIA_OK) {
            HD_LOGE("flush after end of stream failed");
            return false;
        }
        ready_.clear();
        eosQueued_ = false;
        eosSeen_ = false;
        queuedSinceRearm_ = 0;
        return true;
    }

    bool Queue(const uint8_t *data, size_t len, int64_t pts, uint32_t flags)
    {
        ssize_t idx = -1;
        for (int i = 0; i < INPUT_TRIES && idx < 0; i++) {
            idx = ndk_->dequeueInputBuffer(codec_, INPUT_WAIT_US);
            if (idx < 0) {
                Drain(0); /* make room: finished pictures hold output buffers */
            }
        }
        if (idx < 0) {
            HD_LOGE("no input buffer");
            return false;
        }
        size_t cap = 0;
        uint8_t *dst = ndk_->getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
        if (dst == nullptr || cap < len) {
            HD_LOGE("access unit of %{public}zu bytes does not fit a %{public}zu-byte input buffer", len, cap);
            (void)ndk_->queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, static_cast<uint64_t>(pts), 0);
            return false;
        }
        if (len > 0) {
            memcpy(dst, data, len);
        }
        media_status_t st = ndk_->queueInputBuffer(codec_, static_cast<size_t>(idx), 0, len,
                                                   static_cast<uint64_t>(pts), flags);
        if (st != AMEDIA_OK) {
            HD_LOGE("queueInputBuffer failed: %{public}d", st);
            return false;
        }
        lastPts_ = pts;
        if (len > 0) {
            queuedSinceRearm_++;
        }
        return true;
    }

    void ReadLayout()
    {
        AMediaFormat *fmt = ndk_->getOutputFormat(codec_);
        if (fmt == nullptr) {
            return;
        }
        OutLayout l;
        int32_t v = 0;
        if (ndk_->formatGetInt32(fmt, "color-format", &v)) {
            l.color = v;
        }
        (void)ndk_->formatGetInt32(fmt, "width", &l.width);
        (void)ndk_->formatGetInt32(fmt, "height", &l.height);
        l.stride = ndk_->formatGetInt32(fmt, "stride", &v) && v > 0 ? v : l.width;
        l.slice = ndk_->formatGetInt32(fmt, "slice-height", &v) && v > 0 ? v : l.height;
        l.right = l.width - 1;
        l.bottom = l.height - 1;
        int32_t cl = 0;
        int32_t ct = 0;
        int32_t cr = 0;
        int32_t cb = 0;
        if (ndk_->formatGetRect != nullptr && ndk_->formatGetRect(fmt, "crop", &cl, &ct, &cr, &cb) &&
            cr >= cl && cb >= ct) {
            l.left = cl;
            l.top = ct;
            l.right = cr;
            l.bottom = cb;
        }
        HD_LOGI("output: %{public}s", ndk_->formatToString(fmt));
        (void)ndk_->formatDelete(fmt);
        layout_ = l;
    }

    /* Copy one decoded buffer into a tight 8-bit I420 picture. */
    bool Convert(const uint8_t *src, size_t size, Picture &pic) const
    {
        const OutLayout &l = layout_;
        const int32_t w = (l.right - l.left + 1) & ~1;
        const int32_t h = (l.bottom - l.top + 1) & ~1;
        if (w <= 0 || h <= 0 || l.stride <= 0 || l.slice <= 0) {
            return false;
        }
        const int32_t cw = w / 2;
        const int32_t ch = h / 2;
        pic.width = w;
        pic.height = h;
        pic.yuv.resize(static_cast<size_t>(w) * h * 3 / 2);
        uint8_t *py = pic.yuv.data();
        uint8_t *pu = py + static_cast<size_t>(w) * h;
        uint8_t *pv = pu + static_cast<size_t>(cw) * ch;

        const bool p010 = l.color == COLOR_YUVP010;
        const int32_t bpp = p010 ? 2 : 1;
        const size_t lumaPlane = static_cast<size_t>(l.stride) * l.slice; /* stride is in bytes */
        auto need = [&](size_t end) { return end <= size; };
        if (!need(static_cast<size_t>(l.stride) * (l.top + h))) {
            return false;
        }
        for (int32_t y = 0; y < h; y++) {
            const uint8_t *s = src + static_cast<size_t>(l.stride) * (l.top + y) + static_cast<size_t>(l.left) * bpp;
            uint8_t *d = py + static_cast<size_t>(y) * w;
            if (p010) {
                for (int32_t x = 0; x < w; x++) {
                    d[x] = s[2 * x + 1]; /* little-endian 10-in-16: high byte */
                }
            } else {
                memcpy(d, s, w);
            }
        }
        const int32_t cTop = l.top / 2;
        const int32_t cLeft = l.left / 2;
        if (l.color == COLOR_YUV420_PLANAR || l.color == COLOR_YUV420_FLEXIBLE) {
            const int32_t cstride = l.stride / 2;
            const size_t chromaPlane = static_cast<size_t>(cstride) * (l.slice / 2);
            if (!need(lumaPlane + 2 * chromaPlane)) {
                return false;
            }
            const uint8_t *su = src + lumaPlane;
            const uint8_t *sv = su + chromaPlane;
            for (int32_t y = 0; y < ch; y++) {
                memcpy(pu + static_cast<size_t>(y) * cw, su + static_cast<size_t>(cstride) * (cTop + y) + cLeft, cw);
                memcpy(pv + static_cast<size_t>(y) * cw, sv + static_cast<size_t>(cstride) * (cTop + y) + cLeft, cw);
            }
            return true;
        }
        if (l.color == COLOR_YUV420_SEMIPLANAR || p010) {
            if (!need(lumaPlane + static_cast<size_t>(l.stride) * (l.slice / 2))) {
                return false;
            }
            const uint8_t *suv = src + lumaPlane;
            for (int32_t y = 0; y < ch; y++) {
                const uint8_t *s = suv + static_cast<size_t>(l.stride) * (cTop + y) +
                    static_cast<size_t>(cLeft) * 2 * bpp;
                uint8_t *du = pu + static_cast<size_t>(y) * cw;
                uint8_t *dv = pv + static_cast<size_t>(y) * cw;
                for (int32_t x = 0; x < cw; x++) {
                    du[x] = s[(2 * x) * bpp + (bpp - 1)];
                    dv[x] = s[(2 * x + 1) * bpp + (bpp - 1)];
                }
            }
            return true;
        }
        if (!unknownWarned_) {
            unknownWarned_ = true;
            HD_LOGE("unsupported output colour format 0x%{public}x", l.color);
        }
        return false;
    }

    /* Collect whatever pictures are ready (waiting up to waitUs for the first). */
    void Drain(int64_t waitUs)
    {
        for (;;) {
            AMediaCodecBufferInfo info = {};
            ssize_t idx = ndk_->dequeueOutputBuffer(codec_, &info, waitUs);
            if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                ReadLayout();
                continue;
            }
            if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            if (idx < 0) {
                return;
            }
            if (layout_.width == 0) {
                ReadLayout();
            }
            size_t cap = 0;
            uint8_t *buf = ndk_->getOutputBuffer(codec_, static_cast<size_t>(idx), &cap);
            if (buf != nullptr && info.size > 0 && static_cast<size_t>(info.offset) < cap) {
                Picture pic;
                pic.pts = info.presentationTimeUs;
                if (Convert(buf + info.offset, cap - static_cast<size_t>(info.offset), pic)) {
                    ready_.push_back(std::move(pic));
                }
            }
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                eosSeen_ = true;
            }
            (void)ndk_->releaseOutputBuffer(codec_, static_cast<size_t>(idx), false);
            waitUs = 0;
        }
    }

    bool Hand(HEVC_DEC_OUTARGS *out)
    {
        if (ready_.empty()) {
            return false;
        }
        current_ = std::move(ready_.front());
        ready_.pop_front();
        const size_t luma = static_cast<size_t>(current_.width) * current_.height;
        out->uiDecWidth = static_cast<UINT32>(current_.width);
        out->uiDecHeight = static_cast<UINT32>(current_.height);
        out->uiDecStride = static_cast<UINT32>(current_.width);
        out->uiDecBitDepth = 8;
        out->uiTimeStamp = static_cast<UINT64>(current_.pts);
        out->pucOutYUV[0] = current_.yuv.data();
        out->pucOutYUV[1] = current_.yuv.data() + luma;
        out->pucOutYUV[2] = current_.yuv.data() + luma + luma / 4;
        pictures_++;
        return true;
    }

    const MediaNdk *ndk_;
    AMediaCodec *codec_ = nullptr;
    OutLayout layout_;
    std::deque<Picture> ready_;
    Picture current_;
    int64_t lastPts_ = 0;
    const uint8_t *pendingData_ = nullptr; /* sample queued but not yet reported consumed */
    UINT32 pendingLen_ = 0;
    int32_t queuedSinceRearm_ = 0;
    bool eosQueued_ = false;
    bool eosSeen_ = false;
    mutable bool unknownWarned_ = false;
    uint64_t pictures_ = 0;
};

} // namespace
} // namespace OniroCodec
} // namespace OHOS

using namespace OHOS::OniroCodec;

HEVCDEC_EXPORT INT32 HEVC_CreateDecoder(HEVC_DEC_HANDLE *handle, HEVC_DEC_INIT_PARAM *param)
{
    if (handle == nullptr || param == nullptr) {
        return RET_ERROR;
    }
    if (param->uiDecodeMode != IHW265_DECODE_VIDEO) {
        HD_LOGE("only video decoding is supported (mode %{public}d)", static_cast<int>(param->uiDecodeMode));
        return RET_ERROR;
    }
    const MediaNdk *ndk = LoadMediaNdk();
    if (ndk == nullptr) {
        return RET_ERROR;
    }
    auto *s = new (std::nothrow) Session(ndk);
    if (s == nullptr) {
        return RET_ERROR;
    }
    if (!s->Open()) {
        delete s;
        return RET_ERROR;
    }
    HD_LOGI("decoder ready (output order %{public}u)", param->eOutPutOrder);
    *handle = s;
    return 0;
}

HEVCDEC_EXPORT INT32 HEVC_DecodeFrame(HEVC_DEC_HANDLE handle, HEVC_DEC_INARGS *in, HEVC_DEC_OUTARGS *out)
{
    auto *s = static_cast<Session *>(handle);
    if (s == nullptr || out == nullptr) {
        return RET_ERROR;
    }
    return s->Decode(in, out);
}

HEVCDEC_EXPORT INT32 HEVC_FlushFrame(HEVC_DEC_HANDLE handle, HEVC_DEC_OUTARGS *out)
{
    auto *s = static_cast<Session *>(handle);
    if (s == nullptr || out == nullptr) {
        return RET_ERROR;
    }
    return s->Flush(out);
}

HEVCDEC_EXPORT INT32 HEVC_DeleteDecoder(HEVC_DEC_HANDLE handle)
{
    auto *s = static_cast<Session *>(handle);
    if (s == nullptr) {
        return RET_ERROR;
    }
    HD_LOGI("released after %{public}llu picture(s)", static_cast<unsigned long long>(s->Pictures()));
    delete s;
    return 0;
}
