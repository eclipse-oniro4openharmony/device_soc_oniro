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

#include "hybris_binder_ndk.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <dlfcn.h>

#include "hybris_ril_dl.h"
#include "hybris_ril_log.h"

namespace {

const char *const g_symbolNames[] = {
#define HYBRIS_BINDER_NDK_SYM(name) name,
#include "binder_ndk_symbols.inc"
#undef HYBRIS_BINDER_NDK_SYM
};
constexpr size_t SYMBOL_COUNT = sizeof(g_symbolNames) / sizeof(g_symbolNames[0]);

} // namespace

/*
 * The slot table the trampolines tail-jump through.  Hidden visibility lets
 * binder_ndk_trampolines.S address it with a plain adrp/:lo12: pair — no GOT
 * indirection, no symbol preemption.  C linkage and file scope: the assembly
 * refers to it by this unmangled name.
 */
extern "C" {
__attribute__((visibility("hidden"))) void *g_hybris_binder_ndk_fn[SYMBOL_COUNT];

/* Resolved out of the container's libbinder_ndk by Bind() below, then called
 * through the trampolines like any other NDK entry point. */
bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_startThreadPool(void);

/* Landing pad for symbols the container's libbinder_ndk does not export.
 * Reached by a tail jump from a trampoline, so the caller's arguments are
 * still in their registers and irrelevant — we only ever abort.  Better than
 * jumping to NULL: the message names the failure. */
void HybrisBinderNdkMissing(void);
}

namespace OHOS {
namespace HybrisRil {
namespace {

/* Reached through libhybris so the bionic linker resolves its own dependency
 * chain (libbinder, libutils, libc++, liblog) inside the Android tree. */
constexpr const char *BINDER_NDK_SO = "libbinder_ndk.so";

/* Not std::call_once: a first attempt that fails (no /android mount yet, say)
 * must be repeatable, or the table stays full of NULLs forever. */
std::mutex g_bindLock;
std::atomic<bool> g_ok{false};

void Bind()
{
    void *lib = HybrisDlopen(BINDER_NDK_SO, RTLD_NOW);
    if (lib == nullptr) {
        HR_LOGE("hybris_dlopen(%{public}s) failed: %{public}s", BINDER_NDK_SO, HybrisDlerror());
        return;
    }

    size_t missing = 0;
    for (size_t i = 0; i < SYMBOL_COUNT; i++) {
        void *fn = HybrisDlsym(lib, g_symbolNames[i]);
        if (fn == nullptr) {
            fn = reinterpret_cast<void *>(&HybrisBinderNdkMissing);
            missing++;
            HR_LOGW("libbinder_ndk has no %{public}s", g_symbolNames[i]);
        }
        g_hybris_binder_ndk_fn[i] = fn;
    }

    HR_LOGI("bound %{public}zu/%{public}zu libbinder_ndk symbols", SYMBOL_COUNT - missing,
            SYMBOL_COUNT);
    g_ok.store(true);
}

} // namespace

bool BinderNdkInit()
{
    if (g_ok.load()) {
        return true;
    }
    std::lock_guard<std::mutex> guard(g_bindLock);
    if (!g_ok.load()) {
        Bind();
    }
    return g_ok.load();
}

void BinderNdkStartThreadPool(int maxThreads)
{
    if (!BinderNdkInit()) {
        return;
    }
    if (maxThreads > 0) {
        (void)ABinderProcess_setThreadPoolMaxThreadCount(static_cast<uint32_t>(maxThreads));
    }
    ABinderProcess_startThreadPool();
}

} // namespace HybrisRil
} // namespace OHOS

/*
 * The generated AIDL code registers its binder classes from *static*
 * initializers:
 *
 *     static AIBinder_Class *_g_aidl_..._clazz =
 *         ::ndk::ICInterface::defineClass(descriptor, onTransact);
 *
 * so the slot table has to be live before any C++ static init in this module
 * runs — otherwise the very first trampoline branches through a NULL slot
 * and the process dies in _GLOBAL__sub_I_IRadioConfig.cpp before main().
 * A prioritised constructor lands in .init_array.00101, which the linker
 * orders ahead of every unprioritised entry, including the generated ones.
 *
 * This needs only that /android is mounted and libhybris can load the
 * container's libbinder_ndk — AIBinder_Class_define is a purely in-process
 * call.  Neither androidd nor rild need to be up yet.
 */
__attribute__((constructor(101))) static void HybrisBinderNdkEarlyInit(void)
{
    (void)OHOS::HybrisRil::BinderNdkInit();
}

extern "C" void HybrisBinderNdkMissing(void)
{
    HR_LOGE("call into an unresolved libbinder_ndk symbol — aborting");
    (void)fprintf(stderr, "[HybrisRil] call into an unresolved libbinder_ndk symbol\n");
    abort();
}
