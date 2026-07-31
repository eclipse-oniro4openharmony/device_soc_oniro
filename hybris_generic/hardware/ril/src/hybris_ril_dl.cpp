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

#include "hybris_ril_dl.h"

#include <cstdlib>
#include <mutex>

#include <dlfcn.h>

#include "hybris_ril_log.h"

namespace OHOS {
namespace HybrisRil {
namespace {

using FnDlopen = void *(*)(const char *, int);
using FnDlsym = void *(*)(void *, const char *);
using FnDlerror = char *(*)();

FnDlopen g_dlopen = nullptr;
FnDlsym g_dlsym = nullptr;
FnDlerror g_dlerror = nullptr;
std::once_flag g_once;

/*
 * libhybris' default search path is vendor-first, which is right for the
 * vendor HALs the display/audio bridges load.  Everything we need lives in
 * the *system* tree (libbinder_ndk -> libbinder -> libutils -> libc++), and
 * the camera bridge already showed that letting a vendor copy win partway
 * down such a chain produces unresolvable symbols.  The hybris linker reads
 * this variable lazily on first use, so setting it affects only our process.
 */
constexpr const char *HYBRIS_PATH_ENV = "HYBRIS_LD_LIBRARY_PATH";
constexpr const char *RIL_SEARCH_PATH =
    "/android/system/lib64:"
    "/android/vendor/lib64:"
    "/android/vendor/lib64/hw:"
    "/android/odm/lib64:"
    "/apex/com.android.vndk.v34/lib64:"
    "/apex/com.android.i18n/lib64";

void Bind()
{
    if (getenv(HYBRIS_PATH_ENV) == nullptr) {
        (void)setenv(HYBRIS_PATH_ENV, RIL_SEARCH_PATH, 0);
        HR_LOGI("%{public}s set system-first for the radio client stack", HYBRIS_PATH_ENV);
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
        HR_LOGE("dlopen(libhybris-common) failed: %{public}s", dlerror());
        return;
    }
    g_dlopen = reinterpret_cast<FnDlopen>(dlsym(lib, "hybris_dlopen"));
    g_dlsym = reinterpret_cast<FnDlsym>(dlsym(lib, "hybris_dlsym"));
    g_dlerror = reinterpret_cast<FnDlerror>(dlsym(lib, "hybris_dlerror"));
    if (g_dlopen == nullptr || g_dlsym == nullptr) {
        HR_LOGE("libhybris-common is missing hybris_dlopen/hybris_dlsym");
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

} // namespace HybrisRil
} // namespace OHOS
