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
 * Loads Android's NDK media library (libmediandk.so, AMediaCodec) from the
 * Halium image into an OHOS process through libhybris, and gives it what an
 * Android process would have had: binder thread pools and the Android view
 * of a few paths.  Shared by the hybris codec libraries (libavcenc_ohos,
 * libhevcdec_ohos).
 */

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include "hilog/log.h"

#include "mediandk_abi.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002562
#define LOG_TAG "OniroMediaNdk"
#define AE_LOGI(fmt, ...) HILOG_INFO(LOG_CORE, fmt, ##__VA_ARGS__)
#define AE_LOGW(fmt, ...) HILOG_WARN(LOG_CORE, fmt, ##__VA_ARGS__)
#define AE_LOGE(fmt, ...) HILOG_ERROR(LOG_CORE, fmt, ##__VA_ARGS__)

namespace OHOS {
namespace OniroCodec {
namespace {

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
        Resolve(hSym, lib, "AMediaCodec_createDecoderByType", n.createDecoderByType) &&
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
        Resolve(hSym, lib, "AMediaCodec_getOutputFormat", n.getOutputFormat) &&
        Resolve(hSym, lib, "AMediaCodec_flush", n.flush) &&
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
    n.formatGetRect = reinterpret_cast<decltype(n.formatGetRect)>(hSym(lib, "AMediaFormat_getRect"));
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
} // namespace OniroCodec
} // namespace OHOS
