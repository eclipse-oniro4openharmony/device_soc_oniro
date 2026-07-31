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
namespace network {
enum class EmergencyMode : int32_t {
  EMERGENCY_WWAN = 1,
  EMERGENCY_WLAN = 2,
  EMERGENCY_CALLBACK = 3,
};

}  // namespace network
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace network {
[[nodiscard]] static inline std::string toString(EmergencyMode val) {
  switch(val) {
  case EmergencyMode::EMERGENCY_WWAN:
    return "EMERGENCY_WWAN";
  case EmergencyMode::EMERGENCY_WLAN:
    return "EMERGENCY_WLAN";
  case EmergencyMode::EMERGENCY_CALLBACK:
    return "EMERGENCY_CALLBACK";
  default:
    return std::to_string(static_cast<int32_t>(val));
  }
}
}  // namespace network
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace ndk {
namespace internal {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"
template <>
constexpr inline std::array<aidl::android::hardware::radio::network::EmergencyMode, 3> enum_values<aidl::android::hardware::radio::network::EmergencyMode> = {
  aidl::android::hardware::radio::network::EmergencyMode::EMERGENCY_WWAN,
  aidl::android::hardware::radio::network::EmergencyMode::EMERGENCY_WLAN,
  aidl::android::hardware::radio::network::EmergencyMode::EMERGENCY_CALLBACK,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
