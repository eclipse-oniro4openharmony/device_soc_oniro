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
 * avframe_dump: decode a recording with OpenHarmony's own demuxer and video
 * decoder and write chosen frames as tightly packed NV12.
 *
 *   avframe_dump in.mp4 out_prefix frame [frame...]
 *
 * Writes <out_prefix>_<frame>.nv12 and prints each frame's size and pts.
 * Bring-up aid for the recording path (colours, orientation, dropped frames)
 * on a device without any other decoder tooling.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "native_avbuffer.h"
#include "native_avcodec_base.h"
#include "native_avcodec_videodecoder.h"
#include "native_avdemuxer.h"
#include "native_avformat.h"
#include "native_avsource.h"

namespace {

constexpr int64_t QUERY_TIMEOUT_US = 10000;

bool WriteFrame(const std::string &path, const uint8_t *src, int32_t w, int32_t h, int32_t stride,
                int32_t slice)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        perror(path.c_str());
        return false;
    }
    for (int32_t y = 0; y < h; y++) {
        fwrite(src + static_cast<size_t>(y) * stride, 1, w, fp);
    }
    const uint8_t *uv = src + static_cast<size_t>(stride) * slice;
    for (int32_t y = 0; y < h / 2; y++) {
        fwrite(uv + static_cast<size_t>(y) * stride, 1, w, fp);
    }
    fclose(fp);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s in.mp4 out_prefix frame [frame...]\n", argv[0]);
        return 2;
    }
    std::set<int64_t> wanted;
    int64_t last = 0;
    for (int i = 3; i < argc; i++) {
        int64_t n = atoll(argv[i]);
        wanted.insert(n);
        last = n > last ? n : last;
    }
    int fd = open(argv[1], O_RDONLY);
    struct stat st {};
    if (fd < 0 || fstat(fd, &st) != 0) {
        perror(argv[1]);
        return 1;
    }
    OH_AVSource *source = OH_AVSource_CreateWithFD(fd, 0, st.st_size);
    OH_AVDemuxer *demuxer = source ? OH_AVDemuxer_CreateWithSource(source) : nullptr;
    if (demuxer == nullptr) {
        fprintf(stderr, "cannot open source\n");
        return 1;
    }
    OH_AVFormat *srcFmt = OH_AVSource_GetSourceFormat(source);
    int32_t tracks = 0;
    OH_AVFormat_GetIntValue(srcFmt, OH_MD_KEY_TRACK_COUNT, &tracks);
    int32_t vt = -1;
    OH_AVFormat *trackFmt = nullptr;
    for (int32_t t = 0; t < tracks; t++) {
        OH_AVFormat *f = OH_AVSource_GetTrackFormat(source, t);
        int32_t type = -1;
        OH_AVFormat_GetIntValue(f, OH_MD_KEY_TRACK_TYPE, &type);
        if (type == MEDIA_TYPE_VID && vt < 0) {
            vt = t;
            trackFmt = f;
        } else {
            OH_AVFormat_Destroy(f);
        }
    }
    if (vt < 0) {
        fprintf(stderr, "no video track\n");
        return 1;
    }
    const char *mime = nullptr;
    OH_AVFormat_GetStringValue(trackFmt, OH_MD_KEY_CODEC_MIME, &mime);
    printf("video track %d: %s\n", vt, OH_AVFormat_DumpInfo(trackFmt));
    OH_AVDemuxer_SelectTrackByID(demuxer, vt);

    OH_AVCodec *dec = OH_VideoDecoder_CreateByMime(mime);
    if (dec == nullptr) {
        fprintf(stderr, "no decoder for %s\n", mime ? mime : "?");
        return 1;
    }
    OH_AVFormat_SetIntValue(trackFmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetIntValue(trackFmt, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
    if (OH_VideoDecoder_Configure(dec, trackFmt) != AV_ERR_OK || OH_VideoDecoder_Prepare(dec) != AV_ERR_OK ||
        OH_VideoDecoder_Start(dec) != AV_ERR_OK) {
        fprintf(stderr, "decoder setup failed\n");
        return 1;
    }

    bool inputDone = false;
    int64_t frame = 0;
    while (frame <= last) {
        uint32_t idx = 0;
        if (!inputDone && OH_VideoDecoder_QueryInputBuffer(dec, &idx, QUERY_TIMEOUT_US) == AV_ERR_OK) {
            OH_AVBuffer *in = OH_VideoDecoder_GetInputBuffer(dec, idx);
            OH_AVDemuxer_ReadSampleBuffer(demuxer, vt, in);
            OH_AVCodecBufferAttr attr {};
            OH_AVBuffer_GetBufferAttr(in, &attr);
            inputDone = (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0;
            OH_VideoDecoder_PushInputBuffer(dec, idx);
        }
        if (OH_VideoDecoder_QueryOutputBuffer(dec, &idx, QUERY_TIMEOUT_US) != AV_ERR_OK) {
            continue;
        }
        OH_AVBuffer *out = OH_VideoDecoder_GetOutputBuffer(dec, idx);
        OH_AVCodecBufferAttr attr {};
        OH_AVBuffer_GetBufferAttr(out, &attr);
        if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
            OH_VideoDecoder_FreeOutputBuffer(dec, idx);
            break;
        }
        if (wanted.count(frame) != 0) {
            OH_AVFormat *of = OH_VideoDecoder_GetOutputDescription(dec);
            int32_t w = 0;
            int32_t h = 0;
            int32_t stride = 0;
            int32_t slice = 0;
            OH_AVFormat_GetIntValue(of, OH_MD_KEY_VIDEO_PIC_WIDTH, &w);
            OH_AVFormat_GetIntValue(of, OH_MD_KEY_VIDEO_PIC_HEIGHT, &h);
            OH_AVFormat_GetIntValue(of, OH_MD_KEY_VIDEO_STRIDE, &stride);
            OH_AVFormat_GetIntValue(of, OH_MD_KEY_VIDEO_SLICE_HEIGHT, &slice);
            if (w <= 0 || h <= 0) {
                OH_AVFormat_GetIntValue(of, OH_MD_KEY_WIDTH, &w);
                OH_AVFormat_GetIntValue(of, OH_MD_KEY_HEIGHT, &h);
            }
            stride = stride > 0 ? stride : w;
            slice = slice > 0 ? slice : h;
            std::string path = std::string(argv[2]) + "_" + std::to_string(frame) + ".nv12";
            if (WriteFrame(path, OH_AVBuffer_GetAddr(out), w, h, stride, slice)) {
                printf("frame %lld pts %lld %dx%d stride %d/%d -> %s\n", static_cast<long long>(frame),
                       static_cast<long long>(attr.pts), w, h, stride, slice, path.c_str());
            }
            OH_AVFormat_Destroy(of);
        }
        OH_VideoDecoder_FreeOutputBuffer(dec, idx);
        frame++;
    }
    printf("%lld frames decoded\n", static_cast<long long>(frame));
    OH_VideoDecoder_Stop(dec);
    OH_VideoDecoder_Destroy(dec);
    OH_AVDemuxer_Destroy(demuxer);
    OH_AVSource_Destroy(source);
    close(fd);
    return 0;
}
