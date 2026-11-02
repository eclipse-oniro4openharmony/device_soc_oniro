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

#ifndef HYBRIS_GNSS_LEGACY_H
#define HYBRIS_GNSS_LEGACY_H

#include <memory>

#include <aidl/android/hardware/gnss/IAGnss.h>
#include <aidl/android/hardware/gnss/IAGnssCallback.h>
#include <aidl/android/hardware/gnss/IAGnssRil.h>
#include <aidl/android/hardware/gnss/IAGnssRilCallback.h>
#include <aidl/android/hardware/gnss/IGnss.h>
#include <aidl/android/hardware/gnss/IGnssCallback.h>

namespace OHOS {
namespace HybrisGnss {

namespace gnss = ::aidl::android::hardware::gnss;

/*
 * Load the vendor's legacy GPS HAL module (gps.default.so) through libhybris
 * and wrap it as an in-process IGnss (with IAGnss / IAGnssRil extensions when
 * the module offers "agps" / "agps_ril").  nullptr if there is no usable
 * module.  See hybris_gnss_legacy.cpp.
 */
std::shared_ptr<gnss::IGnss> OpenLegacyGnss();

} // namespace HybrisGnss
} // namespace OHOS

#endif // HYBRIS_GNSS_LEGACY_H
