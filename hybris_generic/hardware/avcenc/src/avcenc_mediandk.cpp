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
 * libavcenc_ohos.z.so: av_codec's H.264 encoder, on the phone's hardware.
 *
 * av_codec's AvcEncoder (foundation/multimedia/av_codec/services/engine/
 * codec/video/avcencoder) dlopens this library for three entry points:
 *
 *   uint32_t InitEncoder(AVC_ENC_HANDLE *, AVC_ENC_INIT_PARAM *);
 *   uint32_t EncodeProcess(AVC_ENC_HANDLE, AVC_ENC_INARGS *, AVC_ENC_OUTARGS *);
 *   uint32_t ReleaseEncoder(AVC_ENC_HANDLE);
 *
 * and only offers video/avc encoding when the library exists.  Upstream
 * expects a closed vendor blob there.  This one drives the Android side's
 * MediaCodec encoder (MediaTek's Codec2 c2.mtk.avc.encoder on the Volla
 * phones) through the NDK's synchronous AMediaCodec API in libmediandk.so,
 * loaded from the Halium system image with libhybris.
 *
 * av_codec drives the library strictly one frame in, one access unit out:
 *
 *  - The first EncodeProcess has no input: it is the header request and
 *    must return SPS + PPS (frame type 0, codec data).  The encoder only
 *    emits its codec config once it has a frame, so InitEncoder takes the
 *    header from a throwaway instance with the same format, fed one grey
 *    frame, before opening the real one.
 *  - Later calls with no input come from end of stream.
 *  - Input is planar I420 in inputBufs[0..2] with strides; the codec is
 *    configured for COLOR_FormatYUV420Planar, so frames copy straight in.
 *  - iperiod is the GOP length in frames (0: every frame is a key frame);
 *    idrRequest asks for a key frame now (request-sync).
 *  - Output is Annex B, which is what MediaCodec's AVC encoders produce.
 *  - With P frames the encoder holds three frames before its first output;
 *    EncodeProcess pads the first frame to fit the one-call-per-frame ABI.
 *
 * Making libmediandk usable from an OHOS process takes what an Android
 * process gets for free: binder thread pools (StartBinderThreads) and the
 * Android view of a few paths (PATH_REDIRECTS); without them MediaCodecList
 * finds no codecs at all.
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <cstdarg>
#include <cstdio>

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include "hilog/log.h"

#include "AvcEnc_Typedef.h"
#include "mediandk_abi.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002562
#define LOG_TAG "OniroAvcEnc"
#define AE_LOGI(fmt, ...) HILOG_INFO(LOG_CORE, fmt, ##__VA_ARGS__)
#define AE_LOGW(fmt, ...) HILOG_WARN(LOG_CORE, fmt, ##__VA_ARGS__)
#define AE_LOGE(fmt, ...) HILOG_ERROR(LOG_CORE, fmt, ##__VA_ARGS__)

#define AVCENC_EXPORT extern "C" __attribute__((visibility("default")))

namespace OHOS {
namespace OniroAvcEnc {
namespace {

constexpr uint32_t RET_OK = 0;
constexpr uint32_t RET_FAIL = 1;

/* av_codec's AVC_ENCODER_*_FRAMETYPE (avc_encoder_util.cpp). */
constexpr uint32_t FRAMETYPE_P = 1;
constexpr uint32_t FRAMETYPE_I = 3;
constexpr uint32_t FRAMETYPE_CODEC_DATA = 0;

constexpr int32_t DEFAULT_FRAME_RATE = 30;
constexpr int32_t DEFAULT_KEY_INTERVAL_S = 2;

/* MediaCodecInfo.CodecCapabilities / OMX values. */
constexpr int32_t COLOR_YUV420_PLANAR = 19;
constexpr int32_t BITRATE_MODE_VBR = 1;
constexpr int32_t BITRATE_MODE_CBR = 2;
constexpr int32_t AVC_PROFILE_BASELINE = 0x01;
constexpr int32_t AVC_PROFILE_MAIN = 0x02;
constexpr int32_t AVC_PROFILE_HIGH = 0x08;

constexpr const char *MIME_AVC = "video/avc";
/* Tried by name first, so MediaCodecList's ordering cannot hand us a
 * software encoder; ONIRO_AVCENC_CODEC overrides it. */
constexpr const char *PREFERRED_CODEC = "c2.mtk.avc.encoder";

constexpr uint8_t NAL_SPS = 7;
constexpr uint8_t NAL_PPS = 8;

constexpr int64_t INPUT_WAIT_US = 500 * 1000;
constexpr int64_t OUTPUT_POLL_US = 10 * 1000;
constexpr auto FRAME_WAIT = std::chrono::milliseconds(1000);
constexpr auto FIRST_FRAME_WAIT = std::chrono::milliseconds(2000);
constexpr auto HEADER_WAIT = std::chrono::milliseconds(3000);
constexpr int HEADER_MAX_FRAMES = 8;

/* ─── libhybris / libmediandk loading ─────────────────────────────────────── */

using FnDlopen = void *(*)(const char *, int);
using FnDlsym = void *(*)(void *, const char *);
using FnDlerror = char *(*)();

/*
 * Same search order as the camera VDI: libmediandk pulls in libstagefright
 * and libcodec2_vndk, which exist in both the system and the vendor tree, and
 * only the system copies resolve against each other.  The hybris linker reads
 * the variable when it first initialises, so this only takes effect if nothing
 * in this process used libhybris before us.
 */
constexpr const char *HYBRIS_PATH_ENV = "HYBRIS_LD_LIBRARY_PATH";
constexpr const char *SEARCH_PATH =
    "/android/system/lib64:"
    "/android/vendor/lib64:"
    "/android/vendor/lib64/hw:"
    "/android/odm/lib64:"
    "/apex/com.android.vndk.v34/lib64:"
    "/apex/com.android.i18n/lib64";

template <typename T>
bool Resolve(FnDlsym sym, void *lib, const char *name, T &out)
{
    out = reinterpret_cast<T>(sym(lib, name));
    if (out == nullptr) {
        AE_LOGE("libmediandk.so has no %{public}s", name);
        return false;
    }
    return true;
}

/*
 * android::sp<T> as a return value: one pointer, but not trivially copyable,
 * so AArch64 returns it through x8 like the real class.  The reference it
 * carries is deliberately leaked; ProcessState lives as long as the process.
 */
struct SpRet {
    void *ptr = nullptr;
    SpRet() {}
    SpRet(const SpRet &other) : ptr(other.ptr) {}
    ~SpRet() {}
};

/*
 * Binder thread pools for the Android side of this process.  The Codec2 HAL
 * reports finished work through IComponentListener callbacks over hwbinder,
 * and MediaCodec links to death on media.resource_manager over binder; an
 * Android process starts its pools in main(), an OHOS one never does, and
 * without looper threads the encoder takes input and never returns a frame.
 */
void StartBinderThreads(FnDlopen hOpen, FnDlsym hSym)
{
    void *hidl = hOpen("libhidlbase.so", RTLD_NOW);
    if (hidl != nullptr) {
        auto configure = reinterpret_cast<int (*)(size_t, bool)>(
            hSym(hidl, "_ZN7android8hardware22configureRpcThreadpoolEmb"));
        auto self = reinterpret_cast<SpRet (*)()>(hSym(hidl, "_ZN7android8hardware12ProcessState4selfEv"));
        auto start = reinterpret_cast<void (*)(void *)>(
            hSym(hidl, "_ZN7android8hardware12ProcessState15startThreadPoolEv"));
        if (configure != nullptr && self != nullptr && start != nullptr) {
            (void)configure(4, false);
            SpRet ps = self();
            if (ps.ptr != nullptr) {
                start(ps.ptr);
            }
        } else {
            AE_LOGW("libhidlbase.so lacks the thread-pool entry points");
        }
    }
    void *binder = hOpen("libbinder.so", RTLD_NOW);
    if (binder != nullptr) {
        auto self = reinterpret_cast<SpRet (*)()>(hSym(binder, "_ZN7android12ProcessState4selfEv"));
        auto start = reinterpret_cast<void (*)(void *)>(hSym(binder, "_ZN7android12ProcessState15startThreadPoolEv"));
        if (self != nullptr && start != nullptr) {
            SpRet ps = self();
            if (ps.ptr != nullptr) {
                start(ps.ptr);
            }
        }
    }
}

/*
 * Android paths the media stack probes that mean something else on the
 * OHOS side of the filesystem.  Applied only to Android code loaded through
 * libhybris in this process (hybris_set_hook_callback), so neither OHOS code
 * nor the other hybris users (composer, camera, audio...) see them.
 *
 *  - libhidl decides whether HIDL services exist at all by access()ing the
 *    hwservicemanager binary.  It runs in the Halium container, but its path
 *    there is not the path here, so without this every HIDL getService (the
 *    Codec2 store included) is refused: "hwservicemanager is not supported".
 *    The /android/system/bin entry is an absolute symlink that only resolves
 *    inside the container, hence the system_ext target.
 *  - MediaCodecList reads media_codecs*.xml from /vendor/etc, which here is
 *    the OHOS vendor tree; without them it lists no codecs.
 */
struct PathRedirect {
    const char *from;
    const char *to;
    bool prefix;
};
constexpr PathRedirect PATH_REDIRECTS[] = {
    {"/system_ext/bin/hwservicemanager", "/android/system/system_ext/bin/hwservicemanager", false},
    {"/system/system_ext/bin/hwservicemanager", "/android/system/system_ext/bin/hwservicemanager", false},
    {"/system/bin/hwservicemanager", "/android/system/system_ext/bin/hwservicemanager", false},
    {"/vendor/etc/media_codecs", "/android/vendor/etc/media_codecs", true},
};

const char *Redirect(const char *path)
{
    if (path == nullptr) {
        return path;
    }
    for (const auto &r : PATH_REDIRECTS) {
        size_t n = strlen(r.from);
        if (strncmp(path, r.from, n) == 0 && (r.prefix || path[n] == '\0')) {
            static thread_local char buf[PATH_MAX];
            (void)snprintf(buf, sizeof(buf), "%s%s", r.to, path + n);
            return buf;
        }
    }
    return path;
}

/* libhybris' own hooks, which keep its redirect table for everything else. */
struct DefaultHooks {
    int (*access)(const char *, int) = nullptr;
    int (*stat)(const char *, struct stat *) = nullptr;
    int (*lstat)(const char *, struct stat *) = nullptr;
    int (*open)(const char *, int, ...) = nullptr;
    int (*openat)(int, const char *, int, ...) = nullptr;
    FILE *(*fopen)(const char *, const char *) = nullptr;
};
DefaultHooks g_hooks;

int HookAccess(const char *path, int mode)
{
    return g_hooks.access(Redirect(path), mode);
}

int HookStat(const char *path, struct stat *st)
{
    return g_hooks.stat(Redirect(path), st);
}

int HookLstat(const char *path, struct stat *st)
{
    return g_hooks.lstat(Redirect(path), st);
}

int HookOpen(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    return g_hooks.open(Redirect(path), flags, mode);
}

int HookOpenat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    return g_hooks.openat(dirfd, Redirect(path), flags, mode);
}

FILE *HookFopen(const char *path, const char *mode)
{
    return g_hooks.fopen(Redirect(path), mode);
}

void *HookCallback(const char *sym, const char *)
{
    if (sym == nullptr) {
        return nullptr;
    }
    if (strcmp(sym, "access") == 0) {
        return reinterpret_cast<void *>(HookAccess);
    }
    if (strcmp(sym, "stat") == 0) {
        return reinterpret_cast<void *>(HookStat);
    }
    if (strcmp(sym, "lstat") == 0) {
        return reinterpret_cast<void *>(HookLstat);
    }
    if (strcmp(sym, "open") == 0) {
        return reinterpret_cast<void *>(HookOpen);
    }
    if (strcmp(sym, "openat") == 0) {
        return reinterpret_cast<void *>(HookOpenat);
    }
    if (strcmp(sym, "fopen") == 0) {
        return reinterpret_cast<void *>(HookFopen);
    }
    return nullptr;
}

/* Must run before the first hybris_dlopen: symbols bind at load time. */
bool InstallPathHooks(void *hybris)
{
    using SetCb = void (*)(void *(*)(const char *, const char *));
    auto set = reinterpret_cast<SetCb>(dlsym(hybris, "hybris_set_hook_callback"));
    g_hooks.access = reinterpret_cast<decltype(g_hooks.access)>(dlsym(hybris, "_hybris_hook_access"));
    g_hooks.stat = reinterpret_cast<decltype(g_hooks.stat)>(dlsym(hybris, "_hybris_hook_stat"));
    g_hooks.lstat = reinterpret_cast<decltype(g_hooks.lstat)>(dlsym(hybris, "_hybris_hook_lstat"));
    g_hooks.open = reinterpret_cast<decltype(g_hooks.open)>(dlsym(hybris, "_hybris_hook_open"));
    g_hooks.openat = reinterpret_cast<decltype(g_hooks.openat)>(dlsym(hybris, "_hybris_hook_openat"));
    g_hooks.fopen = reinterpret_cast<decltype(g_hooks.fopen)>(dlsym(hybris, "_hybris_hook_fopen"));
    if (set == nullptr || g_hooks.access == nullptr || g_hooks.stat == nullptr || g_hooks.lstat == nullptr ||
        g_hooks.open == nullptr || g_hooks.openat == nullptr || g_hooks.fopen == nullptr) {
        AE_LOGE("libhybris-common lacks the hook API; Codec2 will not be found");
        return false;
    }
    set(HookCallback);
    return true;
}

MediaNdk g_ndk;
bool g_ndkOk = false;
std::once_flag g_ndkOnce;

void LoadOnce()
{
    if (getenv(HYBRIS_PATH_ENV) == nullptr) {
        (void)setenv(HYBRIS_PATH_ENV, SEARCH_PATH, 0);
    }
    static const char *candidates[] = {
        "/system/lib64/libhybris-common.z.so",
        "libhybris-common.z.so",
        "libhybris-common.so",
    };
    void *hybris = nullptr;
    for (const char *path : candidates) {
        hybris = dlopen(path, RTLD_LAZY);
        if (hybris != nullptr) {
            break;
        }
    }
    if (hybris == nullptr) {
        AE_LOGE("dlopen(libhybris-common) failed: %{public}s", dlerror());
        return;
    }
    auto hOpen = reinterpret_cast<FnDlopen>(dlsym(hybris, "hybris_dlopen"));
    auto hSym = reinterpret_cast<FnDlsym>(dlsym(hybris, "hybris_dlsym"));
    auto hErr = reinterpret_cast<FnDlerror>(dlsym(hybris, "hybris_dlerror"));
    if (hOpen == nullptr || hSym == nullptr) {
        AE_LOGE("libhybris-common lacks hybris_dlopen/hybris_dlsym");
        return;
    }
    if (!InstallPathHooks(hybris)) {
        return;
    }
    void *lib = hOpen("libmediandk.so", RTLD_NOW);
    if (lib == nullptr) {
        const char *err = hErr != nullptr ? hErr() : nullptr;
        AE_LOGE("hybris_dlopen(libmediandk.so) failed: %{public}s", err != nullptr ? err : "?");
        return;
    }
    MediaNdk n;
    bool ok = Resolve(hSym, lib, "AMediaCodec_createCodecByName", n.createCodecByName) &&
        Resolve(hSym, lib, "AMediaCodec_createEncoderByType", n.createEncoderByType) &&
        Resolve(hSym, lib, "AMediaCodec_configure", n.configure) &&
        Resolve(hSym, lib, "AMediaCodec_start", n.start) &&
        Resolve(hSym, lib, "AMediaCodec_stop", n.stop) &&
        Resolve(hSym, lib, "AMediaCodec_delete", n.destroy) &&
        Resolve(hSym, lib, "AMediaCodec_dequeueInputBuffer", n.dequeueInputBuffer) &&
        Resolve(hSym, lib, "AMediaCodec_getInputBuffer", n.getInputBuffer) &&
        Resolve(hSym, lib, "AMediaCodec_queueInputBuffer", n.queueInputBuffer) &&
        Resolve(hSym, lib, "AMediaCodec_dequeueOutputBuffer", n.dequeueOutputBuffer) &&
        Resolve(hSym, lib, "AMediaCodec_getOutputBuffer", n.getOutputBuffer) &&
        Resolve(hSym, lib, "AMediaCodec_releaseOutputBuffer", n.releaseOutputBuffer) &&
        Resolve(hSym, lib, "AMediaCodec_getInputFormat", n.getInputFormat) &&
        Resolve(hSym, lib, "AMediaCodec_setParameters", n.setParameters) &&
        Resolve(hSym, lib, "AMediaFormat_new", n.formatNew) &&
        Resolve(hSym, lib, "AMediaFormat_delete", n.formatDelete) &&
        Resolve(hSym, lib, "AMediaFormat_setString", n.formatSetString) &&
        Resolve(hSym, lib, "AMediaFormat_setInt32", n.formatSetInt32) &&
        Resolve(hSym, lib, "AMediaFormat_getInt32", n.formatGetInt32) &&
        Resolve(hSym, lib, "AMediaFormat_toString", n.formatToString);
    if (!ok) {
        return;
    }
    StartBinderThreads(hOpen, hSym);
    g_ndk = n;
    g_ndkOk = true;
    AE_LOGI("libmediandk.so loaded through libhybris");
}

} // namespace

const MediaNdk *LoadMediaNdk()
{
    std::call_once(g_ndkOnce, LoadOnce);
    return g_ndkOk ? &g_ndk : nullptr;
}

namespace {

/* ─── Settings ───────────────────────────────────────────────────────────── */

struct Settings {
    int32_t width = 0;
    int32_t height = 0;
    int32_t fps = DEFAULT_FRAME_RATE;
    int32_t bitrate = 0;
    int32_t bitrateMode = BITRATE_MODE_VBR;
    int32_t profile = AVC_PROFILE_BASELINE;
    int32_t keyIntervalS = DEFAULT_KEY_INTERVAL_S; /* 0: all key frames */
};

struct Frame {
    const uint8_t *plane[3] = {};
    int32_t stride[3] = {};
};

struct Packet {
    std::vector<uint8_t> data;
    int64_t ts = 0;
    bool key = false;
};

using Clock = std::chrono::steady_clock;

/* Walk Annex B start codes; keep SPS and PPS with their start codes. */
void ExtractParameterSets(const uint8_t *p, size_t n, std::vector<uint8_t> &out)
{
    auto startAt = [&](size_t k) -> size_t {
        if (k + 3 <= n && p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1) {
            return 3;
        }
        if (k + 4 <= n && p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 0 && p[k + 3] == 1) {
            return 4;
        }
        return 0;
    };
    size_t i = 0;
    while (i < n) {
        size_t sc = startAt(i);
        if (sc == 0) {
            i++;
            continue;
        }
        size_t j = i + sc;
        while (j < n && startAt(j) == 0) {
            j++;
        }
        if (i + sc < n) {
            uint8_t type = p[i + sc] & 0x1F;
            if (type == NAL_SPS || type == NAL_PPS) {
                out.insert(out.end(), p + i, p + j);
            }
        }
        i = j;
    }
}

/* ─── One AMediaCodec encoder ────────────────────────────────────────────── */

class HwEncoder {
public:
    explicit HwEncoder(const MediaNdk *ndk) : ndk_(ndk) {}

    ~HwEncoder()
    {
        if (codec_ != nullptr) {
            (void)ndk_->stop(codec_);
            (void)ndk_->destroy(codec_);
        }
    }

    bool Open(const Settings &s, bool quiet = false)
    {
        s_ = s;
        const char *name = getenv("ONIRO_AVCENC_CODEC");
        name = (name != nullptr && *name != '\0') ? name : PREFERRED_CODEC;
        codec_ = ndk_->createCodecByName(name);
        if (codec_ == nullptr) {
            AE_LOGW("%{public}s unavailable, asking MediaCodecList for any %{public}s encoder", name, MIME_AVC);
            codec_ = ndk_->createEncoderByType(MIME_AVC);
        }
        if (codec_ == nullptr) {
            AE_LOGE("no %{public}s encoder", MIME_AVC);
            return false;
        }
        AMediaFormat *fmt = ndk_->formatNew();
        ndk_->formatSetString(fmt, "mime", MIME_AVC);
        ndk_->formatSetInt32(fmt, "width", s.width);
        ndk_->formatSetInt32(fmt, "height", s.height);
        ndk_->formatSetInt32(fmt, "color-format", COLOR_YUV420_PLANAR);
        ndk_->formatSetInt32(fmt, "bitrate", s.bitrate);
        ndk_->formatSetInt32(fmt, "bitrate-mode", s.bitrateMode);
        ndk_->formatSetInt32(fmt, "frame-rate", s.fps);
        ndk_->formatSetInt32(fmt, "i-frame-interval", s.keyIntervalS);
        ndk_->formatSetInt32(fmt, "profile", s.profile);
        /* av_codec's ABI is one access unit per frame, in order.  (The MTK
         * encoder still holds 3 frames at start whatever "latency" /
         * "low-latency" / vendor WFD keys say; EncodeProcess deals with it.) */
        ndk_->formatSetInt32(fmt, "max-bframes", 0);
        ApplyExtraKeys(fmt);
        media_status_t st = ndk_->configure(codec_, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        (void)ndk_->formatDelete(fmt);
        if (st != AMEDIA_OK) {
            AE_LOGE("configure(%{public}dx%{public}d) failed: %{public}d", s.width, s.height, st);
            return false;
        }
        AMediaFormat *in = ndk_->getInputFormat(codec_);
        stride_ = s.width;
        sliceHeight_ = s.height;
        if (in != nullptr) {
            int32_t v = 0;
            if (ndk_->formatGetInt32(in, "stride", &v) && v >= s.width) {
                stride_ = v;
            }
            if (ndk_->formatGetInt32(in, "slice-height", &v) && v >= s.height) {
                sliceHeight_ = v;
            }
            if (!quiet) {
                AE_LOGI("input format: %{public}s", ndk_->formatToString(in));
            }
            (void)ndk_->formatDelete(in);
        }
        st = ndk_->start(codec_);
        if (st != AMEDIA_OK) {
            AE_LOGE("start failed: %{public}d", st);
            return false;
        }
        return true;
    }

    bool Queue(const Frame &f, int64_t tsUs)
    {
        ssize_t idx = ndk_->dequeueInputBuffer(codec_, 0);
        if (idx < 0) {
            /* Every input buffer is still in the codec: pull ready output
             * (kept for the following calls), then wait for a buffer. */
            (void)Poll(OUTPUT_POLL_US);
            idx = ndk_->dequeueInputBuffer(codec_, INPUT_WAIT_US);
        }
        if (idx < 0) {
            AE_LOGE("no input buffer (%{public}zd)", idx);
            return false;
        }
        size_t cap = 0;
        uint8_t *dst = ndk_->getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
        const size_t lumaBytes = static_cast<size_t>(stride_) * sliceHeight_;
        const size_t need = lumaBytes * 3 / 2;
        if (dst == nullptr || cap < need) {
            AE_LOGE("input buffer %{public}zd too small (%{public}zu < %{public}zu)", idx, cap, need);
            return false;
        }
        const int32_t w = s_.width;
        const int32_t h = s_.height;
        const int32_t cw = (w + 1) / 2;
        const int32_t ch = (h + 1) / 2;
        for (int32_t y = 0; y < h; y++) {
            memcpy(dst + static_cast<size_t>(y) * stride_, f.plane[0] + static_cast<size_t>(y) * f.stride[0], w);
        }
        const int32_t cstride = stride_ / 2;
        uint8_t *u = dst + lumaBytes;
        uint8_t *v = u + static_cast<size_t>(cstride) * (sliceHeight_ / 2);
        for (int32_t y = 0; y < ch; y++) {
            memcpy(u + static_cast<size_t>(y) * cstride, f.plane[1] + static_cast<size_t>(y) * f.stride[1], cw);
            memcpy(v + static_cast<size_t>(y) * cstride, f.plane[2] + static_cast<size_t>(y) * f.stride[2], cw);
        }
        media_status_t st = ndk_->queueInputBuffer(codec_, static_cast<size_t>(idx), 0, need,
                                                   static_cast<uint64_t>(tsUs), 0);
        if (st != AMEDIA_OK) {
            AE_LOGE("queueInputBuffer failed: %{public}d", st);
            return false;
        }
        return true;
    }

    /* Next encoded access unit (codec config excluded), or false on timeout. */
    bool Next(Packet &out, std::chrono::milliseconds wait)
    {
        const auto deadline = Clock::now() + wait;
        while (packets_.empty() && Clock::now() < deadline) {
            (void)Poll(OUTPUT_POLL_US);
        }
        if (packets_.empty()) {
            return false;
        }
        out = std::move(packets_.front());
        packets_.pop_front();
        return true;
    }

    /* SPS + PPS, from a codec-config buffer or the first key frame. */
    bool CodecConfig(std::vector<uint8_t> &out, std::chrono::milliseconds wait)
    {
        const auto deadline = Clock::now() + wait;
        while (config_.empty() && Clock::now() < deadline) {
            (void)Poll(OUTPUT_POLL_US);
        }
        out = config_;
        return !out.empty();
    }

    void RequestKeyFrame()
    {
        AMediaFormat *p = ndk_->formatNew();
        ndk_->formatSetInt32(p, "request-sync", 0);
        media_status_t st = ndk_->setParameters(codec_, p);
        (void)ndk_->formatDelete(p);
        if (st != AMEDIA_OK) {
            AE_LOGW("request-sync refused: %{public}d", st);
        }
    }

    void SetBitrate(int32_t bitrate)
    {
        AMediaFormat *p = ndk_->formatNew();
        ndk_->formatSetInt32(p, "video-bitrate", bitrate);
        media_status_t st = ndk_->setParameters(codec_, p);
        (void)ndk_->formatDelete(p);
        if (st == AMEDIA_OK) {
            AE_LOGI("bitrate -> %{public}d", bitrate);
        } else {
            AE_LOGW("bitrate change to %{public}d refused: %{public}d", bitrate, st);
        }
    }

    int32_t Stride() const
    {
        return stride_;
    }

    int32_t SliceHeight() const
    {
        return sliceHeight_;
    }

private:
    /* Bring-up aid: ONIRO_AVCENC_EXTRA="key=int,key=int" adds int32 format
     * keys (e.g. MediaTek vendor.mtk.* parameters) without a rebuild. */
    void ApplyExtraKeys(AMediaFormat *fmt) const
    {
        const char *extra = getenv("ONIRO_AVCENC_EXTRA");
        if (extra == nullptr) {
            return;
        }
        std::string all(extra);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(',', pos);
            std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            size_t eq = item.find('=');
            if (eq != std::string::npos) {
                std::string key = item.substr(0, eq);
                int32_t val = static_cast<int32_t>(strtol(item.c_str() + eq + 1, nullptr, 0));
                ndk_->formatSetInt32(fmt, key.c_str(), val);
                AE_LOGI("extra key %{public}s = %{public}d", key.c_str(), val);
            }
            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }
    }

    /* Drain whatever output is ready into packets_/config_. */
    bool Poll(int64_t timeoutUs)
    {
        bool got = false;
        for (;;) {
            AMediaCodecBufferInfo info = {};
            ssize_t idx = ndk_->dequeueOutputBuffer(codec_, &info, timeoutUs);
            if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            if (idx < 0) {
                return got;
            }
            size_t cap = 0;
            uint8_t *buf = ndk_->getOutputBuffer(codec_, static_cast<size_t>(idx), &cap);
            if (buf != nullptr && info.size > 0 && static_cast<size_t>(info.offset + info.size) <= cap) {
                const uint8_t *p = buf + info.offset;
                const size_t n = static_cast<size_t>(info.size);
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                    if (config_.empty()) {
                        config_.assign(p, p + n);
                    }
                } else {
                    if (config_.empty() && (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME)) {
                        ExtractParameterSets(p, n, config_);
                    }
                    Packet pkt;
                    pkt.data.assign(p, p + n);
                    pkt.ts = info.presentationTimeUs;
                    pkt.key = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
                    packets_.push_back(std::move(pkt));
                }
                got = true;
            }
            (void)ndk_->releaseOutputBuffer(codec_, static_cast<size_t>(idx), false);
            timeoutUs = 0; /* take what else is ready, without waiting */
        }
    }

    const MediaNdk *ndk_;
    AMediaCodec *codec_ = nullptr;
    Settings s_;
    int32_t stride_ = 0;
    int32_t sliceHeight_ = 0;
    std::deque<Packet> packets_;
    std::vector<uint8_t> config_;
};

/* ─── The av_codec session ───────────────────────────────────────────────── */

struct Session {
    const MediaNdk *ndk = nullptr;
    Settings s;
    std::unique_ptr<HwEncoder> enc;
    std::vector<uint8_t> header;
    bool headerDone = false;
    int32_t delay = 0;     /* frames the encoder holds before its first output */
    uint64_t queued = 0;   /* frames handed to the encoder, padding included */
    uint64_t returned = 0; /* access units handed back to av_codec */
};

/*
 * SPS + PPS from a throwaway encoder fed mid-grey frames.  It runs before
 * the session's own encoder is opened, so the two never hold hardware
 * encoder sessions at the same time.  How many frames it took to get the
 * config out is the encoder's start-up delay, which EncodeProcess needs.
 */
bool FetchHeader(Session &ss, std::vector<uint8_t> &out, int32_t &delay)
{
    HwEncoder probe(ss.ndk);
    if (!probe.Open(ss.s, true)) {
        return false;
    }
    std::vector<uint8_t> grey(static_cast<size_t>(ss.s.width) * ss.s.height, 0x80);
    Frame f;
    f.plane[0] = grey.data();
    f.plane[1] = grey.data();
    f.plane[2] = grey.data();
    f.stride[0] = ss.s.width;
    f.stride[1] = ss.s.width / 2;
    f.stride[2] = ss.s.width / 2;
    /* The encoder may hold a few frames before its first output (it
     * reports latency 3), so keep feeding grey frames until the config
     * comes out. */
    const auto deadline = Clock::now() + HEADER_WAIT;
    for (int n = 0; Clock::now() < deadline; n++) {
        if (n < HEADER_MAX_FRAMES && !probe.Queue(f, static_cast<int64_t>(n) * 33333)) {
            return false;
        }
        if (probe.CodecConfig(out, std::chrono::milliseconds(30))) {
            delay = n < HEADER_MAX_FRAMES ? n : HEADER_MAX_FRAMES - 1;
            AE_LOGI("header after %{public}d grey frame(s)", n + 1);
            return true;
        }
    }
    return false;
}

uint32_t EncodeHeader(Session &ss, AVC_ENC_OUTARGS *out)
{
    const std::vector<uint8_t> &header = ss.header;
    if (header.size() > out->size) {
        AE_LOGE("header (%{public}zu bytes) does not fit %{public}u", header.size(), out->size);
        return RET_FAIL;
    }
    memcpy(out->streamBuf, header.data(), header.size());
    out->bytes = static_cast<uint32_t>(header.size());
    out->encodedFrameType = FRAMETYPE_CODEC_DATA;
    AE_LOGI("header: %{public}zu bytes", header.size());
    return RET_OK;
}

int32_t ProfileFor(ENC_PROFILE p)
{
    switch (p) {
        case PROFILE_MAIN:
            return AVC_PROFILE_MAIN;
        case PROFILE_HIGH:
            return AVC_PROFILE_HIGH;
        default:
            return AVC_PROFILE_BASELINE;
    }
}

} // namespace
} // namespace OniroAvcEnc
} // namespace OHOS

using namespace OHOS::OniroAvcEnc;

AVCENC_EXPORT uint32_t InitEncoder(AVC_ENC_HANDLE *handle, AVC_ENC_INIT_PARAM *param)
{
    if (handle == nullptr || param == nullptr || param->width < 16 || param->height < 16) {
        return RET_FAIL;
    }
    const MediaNdk *ndk = LoadMediaNdk();
    if (ndk == nullptr) {
        return RET_FAIL;
    }
    auto *ss = new (std::nothrow) Session();
    if (ss == nullptr) {
        return RET_FAIL;
    }
    ss->ndk = ndk;
    Settings &s = ss->s;
    s.width = static_cast<int32_t>(param->width) & ~1;
    s.height = static_cast<int32_t>(param->height) & ~1;
    s.fps = param->frameRate ? static_cast<int32_t>(param->frameRate) : DEFAULT_FRAME_RATE;
    s.bitrate = static_cast<int32_t>(param->bitrate);
    if (s.bitrate <= 0) {
        /* CQP has no NDK equivalent the MTK encoder honours: ~0.15 bit/pixel. */
        s.bitrate = static_cast<int32_t>(static_cast<int64_t>(s.width) * s.height * s.fps * 15 / 100);
    }
    s.bitrateMode = param->encMode == MODE_CBR ? BITRATE_MODE_CBR : BITRATE_MODE_VBR;
    s.profile = ProfileFor(param->profile);
    if (param->iperiod == 0) {
        s.keyIntervalS = 0;
    } else {
        int32_t secs = static_cast<int32_t>((param->iperiod + static_cast<uint32_t>(s.fps) / 2) /
                                            static_cast<uint32_t>(s.fps));
        s.keyIntervalS = secs > 0 ? secs : 1;
    }

    if (!FetchHeader(*ss, ss->header, ss->delay) || ss->header.empty()) {
        AE_LOGE("no SPS/PPS from the encoder");
        delete ss;
        return RET_FAIL;
    }
    ss->enc = std::make_unique<HwEncoder>(ndk);
    if (!ss->enc->Open(s)) {
        delete ss;
        return RET_FAIL;
    }
    AE_LOGI("%{public}dx%{public}d@%{public}d %{public}d bps mode %{public}d profile 0x%{public}x "
            "key every %{public}d s (iperiod %{public}u), input pitch %{public}d/%{public}d, delay %{public}d",
            s.width, s.height, s.fps, s.bitrate, s.bitrateMode, s.profile, s.keyIntervalS, param->iperiod,
            ss->enc->Stride(), ss->enc->SliceHeight(), ss->delay);
    *handle = ss;
    return RET_OK;
}

AVCENC_EXPORT uint32_t EncodeProcess(AVC_ENC_HANDLE handle, AVC_ENC_INARGS *in, AVC_ENC_OUTARGS *out)
{
    auto *ss = static_cast<Session *>(handle);
    if (ss == nullptr || in == nullptr || out == nullptr || out->streamBuf == nullptr) {
        return RET_FAIL;
    }
    out->bytes = 0;
    out->isLast = 0;
    out->timestamp = in->timestamp;

    if (in->inputBufs[0] == nullptr) {
        if (!ss->headerDone) {
            ss->headerDone = true;
            return EncodeHeader(*ss, out);
        }
        out->isLast = 1;
        out->encodedFrameType = FRAMETYPE_P;
        return RET_OK;
    }
    if (in->inputBufs[1] == nullptr || in->inputBufs[2] == nullptr) {
        AE_LOGE("expected planar I420 input");
        return RET_FAIL;
    }

    if (in->configArgs.bitrate != 0 && static_cast<int32_t>(in->configArgs.bitrate) != ss->s.bitrate) {
        ss->s.bitrate = static_cast<int32_t>(in->configArgs.bitrate);
        ss->enc->SetBitrate(ss->s.bitrate);
    }
    if (in->configArgs.idrRequest != 0 && ss->queued > 0) {
        ss->enc->RequestKeyFrame();
    }

    Frame f;
    const int32_t cw = (ss->s.width + 1) / 2;
    f.plane[0] = static_cast<const uint8_t *>(in->inputBufs[0]);
    f.plane[1] = static_cast<const uint8_t *>(in->inputBufs[1]);
    f.plane[2] = static_cast<const uint8_t *>(in->inputBufs[2]);
    f.stride[0] = in->stride[0] ? static_cast<int32_t>(in->stride[0]) : ss->s.width;
    f.stride[1] = in->stride[1] ? static_cast<int32_t>(in->stride[1]) : cw;
    f.stride[2] = in->stride[2] ? static_cast<int32_t>(in->stride[2]) : cw;

    const int64_t ts = static_cast<int64_t>(in->timestamp);
    if (!ss->enc->Queue(f, ts)) {
        return RET_FAIL;
    }
    const bool first = ss->queued == 0;
    ss->queued++;

    /*
     * MediaTek's encoder holds `delay` frames (3, measured by FetchHeader)
     * before its first access unit when it codes P frames, and av_codec only
     * hands over the next frame once this call returns.  An empty answer is
     * not an option: the recorder's buffer queue rejects zero-size samples
     * (leaking the slot) or re-muxes a stale one.  So the first frame is
     * queued `delay` more times, at timestamps inside its own frame
     * interval: the encoder then has enough input to return the first
     * access unit now, the copies become tiny P frames that belong to the
     * stream, and every later call returns a real access unit, `delay`
     * frames late, with its own timestamp.  The last `delay` frames are
     * still in the encoder at end of stream and are lost (av_codec's EOS
     * call discards its output).
     */
    if (first && ss->delay > 0) {
        const int64_t step = 1000000 / ss->s.fps / (ss->delay + 1);
        for (int32_t k = 1; k <= ss->delay; k++) {
            if (!ss->enc->Queue(f, ts + step * k)) {
                return RET_FAIL;
            }
            ss->queued++;
        }
    }
    Packet pkt;
    if (!ss->enc->Next(pkt, first ? FIRST_FRAME_WAIT : FRAME_WAIT)) {
        AE_LOGE("no output with %{public}llu frames in the encoder",
                static_cast<unsigned long long>(ss->queued - ss->returned));
        return RET_FAIL;
    }
    ss->returned++;
    if (pkt.data.size() > out->size) {
        AE_LOGE("%{public}zu bytes do not fit the %{public}u-byte output buffer", pkt.data.size(), out->size);
        return RET_FAIL;
    }
    memcpy(out->streamBuf, pkt.data.data(), pkt.data.size());
    out->bytes = static_cast<uint32_t>(pkt.data.size());
    out->timestamp = static_cast<uint64_t>(pkt.ts);
    out->encodedFrameType = pkt.key ? FRAMETYPE_I : FRAMETYPE_P;
    return RET_OK;
}

AVCENC_EXPORT uint32_t ReleaseEncoder(AVC_ENC_HANDLE handle)
{
    auto *ss = static_cast<Session *>(handle);
    if (ss == nullptr) {
        return RET_FAIL;
    }
    AE_LOGI("released: %{public}llu frames in (padding included), %{public}llu out",
            static_cast<unsigned long long>(ss->queued), static_cast<unsigned long long>(ss->returned));
    delete ss;
    return RET_OK;
}
