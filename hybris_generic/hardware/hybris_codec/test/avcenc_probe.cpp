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
 * avcenc_probe: drive libavcenc_ohos.z.so the way av_codec does, on a
 * synthetic moving picture, and write the Annex B stream to a file.
 *
 *   avcenc_probe [width height frames fps bitrate out.h264 iperiod]
 *
 * Defaults: 1280 720 90 30 4000000 /data/local/tmp/avcenc_probe.h264.
 * Prints the header size, each frame's type/size/latency and the overall
 * frame rate; check the file on a host with ffprobe / ffplay.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dlfcn.h>

#include "AvcEnc_Typedef.h"

using InitFn = uint32_t (*)(AVC_ENC_HANDLE *, AVC_ENC_INIT_PARAM *);
using EncodeFn = uint32_t (*)(AVC_ENC_HANDLE, AVC_ENC_INARGS *, AVC_ENC_OUTARGS *);
using ReleaseFn = uint32_t (*)(AVC_ENC_HANDLE);

static void Draw(std::vector<uint8_t> &y, std::vector<uint8_t> &u, std::vector<uint8_t> &v,
                 int w, int h, int n)
{
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            y[static_cast<size_t>(r) * w + c] = static_cast<uint8_t>((c + r + n * 4) & 0xFF);
        }
    }
    int cw = w / 2;
    int ch = h / 2;
    for (int r = 0; r < ch; r++) {
        for (int c = 0; c < cw; c++) {
            u[static_cast<size_t>(r) * cw + c] = static_cast<uint8_t>(64 + (c * 128) / cw);
            v[static_cast<size_t>(r) * cw + c] = static_cast<uint8_t>(64 + (r * 128) / ch);
        }
    }
    /* A white square crossing the picture. */
    int side = h / 6;
    int x0 = (n * 8) % (w - side);
    int y0 = (h - side) / 2;
    for (int r = y0; r < y0 + side; r++) {
        memset(&y[static_cast<size_t>(r) * w + x0], 235, side);
    }
}

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 1280;
    int h = argc > 2 ? atoi(argv[2]) : 720;
    int frames = argc > 3 ? atoi(argv[3]) : 90;
    int fps = argc > 4 ? atoi(argv[4]) : 30;
    int bitrate = argc > 5 ? atoi(argv[5]) : 4000000;
    const char *path = argc > 6 ? argv[6] : "/data/local/tmp/avcenc_probe.h264";

    void *lib = dlopen("libavcenc_ohos.z.so", RTLD_NOW);
    if (lib == nullptr) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto init = reinterpret_cast<InitFn>(dlsym(lib, "InitEncoder"));
    auto encode = reinterpret_cast<EncodeFn>(dlsym(lib, "EncodeProcess"));
    auto release = reinterpret_cast<ReleaseFn>(dlsym(lib, "ReleaseEncoder"));
    if (init == nullptr || encode == nullptr || release == nullptr) {
        fprintf(stderr, "missing entry points\n");
        return 1;
    }

    AVC_ENC_INIT_PARAM p = {};
    p.width = static_cast<uint32_t>(w);
    p.height = static_cast<uint32_t>(h);
    p.encMode = MODE_VBR;
    p.frameRate = static_cast<uint32_t>(fps);
    p.bitrate = static_cast<uint32_t>(bitrate);
    p.colorFmt = YUV_420P;
    p.profile = PROFILE_BASE;
    /* av_codec passes the GOP in frames; 0 makes every frame a key frame. */
    p.iperiod = static_cast<uint32_t>(argc > 7 ? atoi(argv[7]) : fps * 2);

    auto t0 = std::chrono::steady_clock::now();
    AVC_ENC_HANDLE enc = nullptr;
    if (init(&enc, &p) != 0 || enc == nullptr) {
        fprintf(stderr, "InitEncoder failed\n");
        return 1;
    }
    auto ms = [](std::chrono::steady_clock::time_point a) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    };
    printf("init %.1f ms\n", ms(t0));

    FILE *fp = fopen(path, "wb");
    if (fp == nullptr) {
        perror(path);
        return 1;
    }
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3 / 2 + 65536);

    AVC_ENC_INARGS in = {};
    AVC_ENC_OUTARGS o = {};
    o.streamBuf = out.data();
    o.size = static_cast<uint32_t>(out.size());
    t0 = std::chrono::steady_clock::now();
    if (encode(enc, &in, &o) != 0) {
        fprintf(stderr, "header failed\n");
        return 1;
    }
    printf("header %u bytes type %u (%.1f ms)\n", o.bytes, o.encodedFrameType, ms(t0));
    fwrite(out.data(), 1, o.bytes, fp);

    std::vector<uint8_t> y(static_cast<size_t>(w) * h);
    std::vector<uint8_t> u(static_cast<size_t>(w / 2) * (h / 2));
    std::vector<uint8_t> v(u.size());
    size_t total = 0;
    int keys = 0;
    auto start = std::chrono::steady_clock::now();
    for (int n = 0; n < frames; n++) {
        Draw(y, u, v, w, h, n);
        in = {};
        in.colorFmt = YUV_420P;
        in.timestamp = static_cast<uint64_t>(n) * 1000000 / fps;
        in.inputBufs[0] = y.data();
        in.inputBufs[1] = u.data();
        in.inputBufs[2] = v.data();
        in.stride[0] = static_cast<uint32_t>(w);
        in.stride[1] = static_cast<uint32_t>(w / 2);
        in.stride[2] = static_cast<uint32_t>(w / 2);
        in.configArgs.idrRequest = n == 0 ? 1 : 0;
        o = {};
        o.streamBuf = out.data();
        o.size = static_cast<uint32_t>(out.size());
        auto tf = std::chrono::steady_clock::now();
        if (encode(enc, &in, &o) != 0) {
            fprintf(stderr, "frame %d failed\n", n);
            break;
        }
        double lat = ms(tf);
        keys += o.encodedFrameType == 3;
        total += o.bytes;
        if (n < 5 || o.encodedFrameType == 3 || n == frames - 1) {
            printf("frame %3d type %u %7u bytes ts %llu in %.1f ms\n", n, o.encodedFrameType, o.bytes,
                   static_cast<unsigned long long>(o.timestamp), lat);
        }
        fwrite(out.data(), 1, o.bytes, fp);
    }
    double secs = ms(start) / 1000.0;
    fclose(fp);
    printf("%d frames in %.2f s = %.1f fps, %zu bytes (%.0f kbit/s at %d fps), %d key frames -> %s\n",
           frames, secs, frames / secs, total, total * 8.0 / 1000 / (frames / static_cast<double>(fps)), fps,
           keys, path);
    release(enc);
    return 0;
}
