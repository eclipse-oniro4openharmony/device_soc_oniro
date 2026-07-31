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
 * Force-included (-include) ahead of the vendored AOSP binder NDK headers so
 * they compile against OHOS/musl instead of bionic.  The headers are used
 * verbatim — the differences are all spelling, not semantics:
 *
 *   __INTRODUCED_IN / __ANDROID_API__  bionic's API-level gating.  We compile
 *       against a fixed Android 14 container, so every guard is satisfied and
 *       the annotations become no-ops.
 *   __BEGIN_DECLS / __END_DECLS        musl's <sys/cdefs.h> normally has them,
 *       but define them defensively — the headers are included from C++ only.
 *
 * Nothing here changes an ABI: the libbinder_ndk C ABI is deliberately opaque
 * (AIBinder/AParcel/AStatus are incomplete types) and the trampolines in
 * binder_ndk_shim/ forward every call into the container's real
 * implementation.
 */

#ifndef HYBRIS_RIL_BINDER_NDK_COMPAT_H
#define HYBRIS_RIL_BINDER_NDK_COMPAT_H

#include <sys/cdefs.h>

#ifndef __ANDROID_API__
#define __ANDROID_API__ 34
#endif

#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api)
#endif

#ifndef __ANDROID_API_Q__
#define __ANDROID_API_Q__ 29
#endif
#ifndef __ANDROID_API_R__
#define __ANDROID_API_R__ 30
#endif
#ifndef __ANDROID_API_S__
#define __ANDROID_API_S__ 31
#endif
#ifndef __ANDROID_API_T__
#define __ANDROID_API_T__ 33
#endif
#ifndef __ANDROID_API_U__
#define __ANDROID_API_U__ 34
#endif

#ifndef __BEGIN_DECLS
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#endif

/* The generated Bn* skeletons only mark themselves @VintfStability when this
 * is defined, and mtkfusionrild rejects callback binders that are not vintf
 * stable.  Defining it here keeps every translation unit consistent. */
#ifndef BINDER_STABILITY_SUPPORT
#define BINDER_STABILITY_SUPPORT
#endif

#endif // HYBRIS_RIL_BINDER_NDK_COMPAT_H
