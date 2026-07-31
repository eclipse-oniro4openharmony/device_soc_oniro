#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_enums.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace config {
enum class MultipleEnabledProfilesMode : int32_t {
  NONE = 0,
  MEP_A1 = 1,
  MEP_A2 = 2,
  MEP_B = 3,
};

}  // namespace config
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace config {
[[nodiscard]] static inline std::string toString(MultipleEnabledProfilesMode val) {
  switch(val) {
  case MultipleEnabledProfilesMode::NONE:
    return "NONE";
  case MultipleEnabledProfilesMode::MEP_A1:
    return "MEP_A1";
  case MultipleEnabledProfilesMode::MEP_A2:
    return "MEP_A2";
  case MultipleEnabledProfilesMode::MEP_B:
    return "MEP_B";
  default:
    return std::to_string(static_cast<int32_t>(val));
  }
}
}  // namespace config
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace ndk {
namespace internal {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"
template <>
constexpr inline std::array<aidl::android::hardware::radio::config::MultipleEnabledProfilesMode, 4> enum_values<aidl::android::hardware::radio::config::MultipleEnabledProfilesMode> = {
  aidl::android::hardware::radio::config::MultipleEnabledProfilesMode::NONE,
  aidl::android::hardware::radio::config::MultipleEnabledProfilesMode::MEP_A1,
  aidl::android::hardware::radio::config::MultipleEnabledProfilesMode::MEP_A2,
  aidl::android::hardware::radio::config::MultipleEnabledProfilesMode::MEP_B,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
