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

#include "hybris_dl.h"

#include <cstdlib>
#include <mutex>

#include <dlfcn.h>

#include "hybris_camera_common.h"

namespace OHOS {
namespace HybrisCamera {
namespace {

using FnDlopen = void *(*)(const char *, int);
using FnDlsym = void *(*)(void *, const char *);
using FnDlerror = char *(*)();

FnDlopen g_dlopen = nullptr;
FnDlsym g_dlsym = nullptr;
FnDlerror g_dlerror = nullptr;
std::once_flag g_once;

/*
 * libhybris' default search path puts /android/vendor/lib64 first, which is
 * right for the vendor HALs the display and audio bridges load.  The camera
 * client stack is the other way round: libcodec2_vndk.so (reached from
 * libmediandk -> libstagefright) exists in *both* trees, and the vendor build
 * of it wants the VNDK build of libui while everything else in the chain wants
 * the system one.  Picking the vendor copy leaves libcodec2_vndk with an
 * unresolvable GraphicBufferMapper::lock and the whole dlopen fails.
 *
 * The hybris linker reads this variable lazily, when it is first initialised
 * by hybris_dlopen, so setting it here only affects our own process — the
 * composer and audio hosts keep the vendor-first default.
 */
constexpr const char *HYBRIS_PATH_ENV = "HYBRIS_LD_LIBRARY_PATH";
constexpr const char *CAMERA_SEARCH_PATH =
    "/android/system/lib64:"
    "/android/vendor/lib64:"
    "/android/vendor/lib64/hw:"
    "/android/odm/lib64:"
    "/apex/com.android.vndk.v34/lib64:"
    "/apex/com.android.i18n/lib64";

void Bind()
{
    if (getenv(HYBRIS_PATH_ENV) == nullptr) {
        (void)setenv(HYBRIS_PATH_ENV, CAMERA_SEARCH_PATH, 0);
        HC_LOGI("%{public}s set to system-first for the camera client stack", HYBRIS_PATH_ENV);
    }

    // Two spellings: the OHOS build installs libhybris-common with the .z.so
    // suffix, but a bind-mounted development copy may not have it.
    static const char *candidates[] = {
        "/system/lib64/libhybris-common.z.so",
        "libhybris-common.z.so",
        "libhybris-common.so",
    };
    void *lib = nullptr;
    for (const char *path : candidates) {
        lib = dlopen(path, RTLD_LAZY);
        if (lib != nullptr) {
            break;
        }
    }
    if (lib == nullptr) {
        HC_LOGE("dlopen(libhybris-common) failed: %{public}s", dlerror());
        return;
    }
    g_dlopen = reinterpret_cast<FnDlopen>(dlsym(lib, "hybris_dlopen"));
    g_dlsym = reinterpret_cast<FnDlsym>(dlsym(lib, "hybris_dlsym"));
    g_dlerror = reinterpret_cast<FnDlerror>(dlsym(lib, "hybris_dlerror"));
    if (g_dlopen == nullptr || g_dlsym == nullptr) {
        HC_LOGE("libhybris-common is missing hybris_dlopen/hybris_dlsym");
        g_dlopen = nullptr;
        g_dlsym = nullptr;
    }
}

} // namespace

void *HybrisDlopen(const char *filename, int flag)
{
    std::call_once(g_once, Bind);
    return g_dlopen != nullptr ? g_dlopen(filename, flag) : nullptr;
}

void *HybrisDlsym(void *handle, const char *symbol)
{
    std::call_once(g_once, Bind);
    return g_dlsym != nullptr ? g_dlsym(handle, symbol) : nullptr;
}

const char *HybrisDlerror()
{
    std::call_once(g_once, Bind);
    if (g_dlerror == nullptr) {
        return "libhybris unavailable";
    }
    const char *err = g_dlerror();
    return err != nullptr ? err : "(no error)";
}

} // namespace HybrisCamera
} // namespace OHOS
