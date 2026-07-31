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
enum class EmergencyScanType : int32_t {
  NO_PREFERENCE = 0,
  LIMITED_SERVICE = 1,
  FULL_SERVICE = 2,
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
[[nodiscard]] static inline std::string toString(EmergencyScanType val) {
  switch(val) {
  case EmergencyScanType::NO_PREFERENCE:
    return "NO_PREFERENCE";
  case EmergencyScanType::LIMITED_SERVICE:
    return "LIMITED_SERVICE";
  case EmergencyScanType::FULL_SERVICE:
    return "FULL_SERVICE";
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
constexpr inline std::array<aidl::android::hardware::radio::network::EmergencyScanType, 3> enum_values<aidl::android::hardware::radio::network::EmergencyScanType> = {
  aidl::android::hardware::radio::network::EmergencyScanType::NO_PREFERENCE,
  aidl::android::hardware::radio::network::EmergencyScanType::LIMITED_SERVICE,
  aidl::android::hardware::radio::network::EmergencyScanType::FULL_SERVICE,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
