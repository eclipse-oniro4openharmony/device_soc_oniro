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
 * hevc_mkclip: make an H.265 test clip on the device.
 *
 *   hevc_mkclip out.mp4|out.h265 [width height frames fps]
 *
 * Encodes a moving test picture with the hardware HEVC encoder (MediaTek's
 * c2.mtk.hevc.encoder through libmediandk, as the hybris codec libraries do)
 * and muxes it into an MP4 with OpenHarmony's AVMuxer, or writes the raw
 * Annex B stream for a .h265 name (the MP4 muxer needs the closed
 * libav_codec_hevc_parser.z.so for H.265 tracks).  Decoding the result
 * (avframe_dump, or the Gallery) exercises libhevcdec_ohos end to end.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "mediandk_abi.h"
#include "native_avbuffer.h"
#include "native_avcodec_base.h"
#include "native_avformat.h"
#include "native_avmuxer.h"

using namespace OHOS::OniroCodec;

namespace {

struct Unit {
    std::vector<uint8_t> data;
    int64_t pts = 0;
    bool key = false;
};

void Draw(uint8_t *f, int w, int h, int n)
{
    uint8_t *y = f;
    uint8_t *u = f + w * h;
    uint8_t *v = u + (w / 2) * (h / 2);
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            y[r * w + c] = static_cast<uint8_t>(((c / 64 + r / 64) & 1) ? 200 : 60);
        }
    }
    for (int r = 0; r < h / 2; r++) {
        for (int c = 0; c < w / 2; c++) {
            u[r * (w / 2) + c] = static_cast<uint8_t>(c * 255 / (w / 2));       /* blue grows to the right */
            v[r * (w / 2) + c] = static_cast<uint8_t>(r * 255 / (h / 2));       /* red grows downwards */
        }
    }
    int side = h / 5;
    int x0 = (n * 12) % (w - side);
    for (int r = (h - side) / 2; r < (h + side) / 2; r++) {
        memset(y + r * w + x0, 235, side);
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.mp4 [width height frames fps]\n", argv[0]);
        return 2;
    }
    int w = argc > 2 ? atoi(argv[2]) : 1280;
    int h = argc > 3 ? atoi(argv[3]) : 720;
    int frames = argc > 4 ? atoi(argv[4]) : 120;
    int fps = argc > 5 ? atoi(argv[5]) : 30;

    const MediaNdk *ndk = LoadMediaNdk();
    if (ndk == nullptr) {
        fprintf(stderr, "libmediandk unavailable\n");
        return 1;
    }
    AMediaCodec *enc = ndk->createCodecByName("c2.mtk.hevc.encoder");
    if (enc == nullptr) {
        enc = ndk->createEncoderByType("video/hevc");
    }
    if (enc == nullptr) {
        fprintf(stderr, "no HEVC encoder\n");
        return 1;
    }
    AMediaFormat *fmt = ndk->formatNew();
    ndk->formatSetString(fmt, "mime", "video/hevc");
    ndk->formatSetInt32(fmt, "width", w);
    ndk->formatSetInt32(fmt, "height", h);
    ndk->formatSetInt32(fmt, "color-format", 19);
    ndk->formatSetInt32(fmt, "bitrate", w * h * fps / 10);
    ndk->formatSetInt32(fmt, "frame-rate", fps);
    ndk->formatSetInt32(fmt, "i-frame-interval", 1);
    ndk->formatSetInt32(fmt, "max-bframes", 0); /* decode order == display order */
    if (ndk->configure(enc, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK ||
        ndk->start(enc) != AMEDIA_OK) {
        fprintf(stderr, "encoder setup failed\n");
        return 1;
    }
    ndk->formatDelete(fmt);

    int32_t stride = w;
    int32_t slice = h;
    AMediaFormat *in = ndk->getInputFormat(enc);
    if (in != nullptr) {
        ndk->formatGetInt32(in, "stride", &stride);
        ndk->formatGetInt32(in, "slice-height", &slice);
        ndk->formatDelete(in);
    }
    std::vector<uint8_t> pic(static_cast<size_t>(w) * h * 3 / 2);
    std::vector<uint8_t> csd;
    std::vector<Unit> units;
    bool eos = false;
    int fed = 0;
    while (!eos) {
        if (fed <= frames) {
            ssize_t idx = ndk->dequeueInputBuffer(enc, 10000);
            if (idx >= 0) {
                size_t cap = 0;
                uint8_t *dst = ndk->getInputBuffer(enc, static_cast<size_t>(idx), &cap);
                int64_t pts = static_cast<int64_t>(fed) * 1000000 / fps;
                if (fed == frames) {
                    ndk->queueInputBuffer(enc, static_cast<size_t>(idx), 0, 0, static_cast<uint64_t>(pts),
                                          AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                } else {
                    Draw(pic.data(), w, h, fed);
                    for (int r = 0; r < h; r++) {
                        memcpy(dst + r * stride, pic.data() + r * w, w);
                    }
                    uint8_t *du = dst + stride * slice;
                    uint8_t *dv = du + (stride / 2) * (slice / 2);
                    for (int r = 0; r < h / 2; r++) {
                        memcpy(du + r * (stride / 2), pic.data() + w * h + r * (w / 2), w / 2);
                        memcpy(dv + r * (stride / 2), pic.data() + w * h + (w / 2) * (h / 2) + r * (w / 2), w / 2);
                    }
                    ndk->queueInputBuffer(enc, static_cast<size_t>(idx), 0,
                                          static_cast<size_t>(stride) * slice * 3 / 2,
                                          static_cast<uint64_t>(pts), 0);
                }
                fed++;
            }
        }
        AMediaCodecBufferInfo info = {};
        ssize_t o = ndk->dequeueOutputBuffer(enc, &info, 10000);
        if (o < 0) {
            continue;
        }
        size_t cap = 0;
        uint8_t *buf = ndk->getOutputBuffer(enc, static_cast<size_t>(o), &cap);
        if (buf != nullptr && info.size > 0) {
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                csd.assign(buf + info.offset, buf + info.offset + info.size);
            } else {
                Unit unit;
                unit.data.assign(buf + info.offset, buf + info.offset + info.size);
                unit.pts = info.presentationTimeUs;
                unit.key = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
                units.push_back(std::move(unit));
            }
        }
        eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        ndk->releaseOutputBuffer(enc, static_cast<size_t>(o), false);
    }
    ndk->stop(enc);
    ndk->destroy(enc);
    printf("encoded %zu access units, codec config %zu bytes\n", units.size(), csd.size());

    const std::string out(argv[1]);
    if (out.size() > 5 && out.compare(out.size() - 5, 5, ".h265") == 0) {
        /* Raw Annex B: parameter sets, then every access unit. */
        FILE *fp = fopen(out.c_str(), "wb");
        if (fp == nullptr) {
            perror(argv[1]);
            return 1;
        }
        fwrite(csd.data(), 1, csd.size(), fp);
        for (const Unit &unit : units) {
            fwrite(unit.data.data(), 1, unit.data.size(), fp);
        }
        fclose(fp);
        printf("wrote %s (raw Annex B, %dx%d)\n", argv[1], w, h);
        return 0;
    }
    int fd = open(argv[1], O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    OH_AVMuxer *mux = OH_AVMuxer_Create(fd, AV_OUTPUT_FORMAT_MPEG_4);
    OH_AVFormat *track = OH_AVFormat_CreateVideoFormat("video/hevc", w, h);
    OH_AVFormat_SetBuffer(track, OH_MD_KEY_CODEC_CONFIG, csd.data(), csd.size());
    int32_t trackId = -1;
    if (mux == nullptr || OH_AVMuxer_AddTrack(mux, &trackId, track) != AV_ERR_OK ||
        OH_AVMuxer_Start(mux) != AV_ERR_OK) {
        fprintf(stderr, "muxer setup failed\n");
        return 1;
    }
    for (const Unit &unit : units) {
        OH_AVBuffer *b = OH_AVBuffer_Create(static_cast<int32_t>(unit.data.size()));
        memcpy(OH_AVBuffer_GetAddr(b), unit.data.data(), unit.data.size());
        OH_AVCodecBufferAttr attr {};
        attr.pts = unit.pts;
        attr.size = static_cast<int32_t>(unit.data.size());
        attr.flags = unit.key ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
        OH_AVBuffer_SetBufferAttr(b, &attr);
        if (OH_AVMuxer_WriteSampleBuffer(mux, static_cast<uint32_t>(trackId), b) != AV_ERR_OK) {
            fprintf(stderr, "write sample at %lld failed\n", static_cast<long long>(unit.pts));
        }
        OH_AVBuffer_Destroy(b);
    }
    OH_AVMuxer_Stop(mux);
    OH_AVMuxer_Destroy(mux);
    OH_AVFormat_Destroy(track);
    close(fd);
    printf("wrote %s (%dx%d, %d fps)\n", argv[1], w, h, fps);
    return 0;
}
