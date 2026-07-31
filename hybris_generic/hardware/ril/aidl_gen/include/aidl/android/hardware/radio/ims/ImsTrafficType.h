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
namespace ims {
enum class ImsTrafficType : int32_t {
  EMERGENCY = 0,
  EMERGENCY_SMS = 1,
  VOICE = 2,
  VIDEO = 3,
  SMS = 4,
  REGISTRATION = 5,
  UT_XCAP = 6,
};

}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
[[nodiscard]] static inline std::string toString(ImsTrafficType val) {
  switch(val) {
  case ImsTrafficType::EMERGENCY:
    return "EMERGENCY";
  case ImsTrafficType::EMERGENCY_SMS:
    return "EMERGENCY_SMS";
  case ImsTrafficType::VOICE:
    return "VOICE";
  case ImsTrafficType::VIDEO:
    return "VIDEO";
  case ImsTrafficType::SMS:
    return "SMS";
  case ImsTrafficType::REGISTRATION:
    return "REGISTRATION";
  case ImsTrafficType::UT_XCAP:
    return "UT_XCAP";
  default:
    return std::to_string(static_cast<int32_t>(val));
  }
}
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace ndk {
namespace internal {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"
template <>
constexpr inline std::array<aidl::android::hardware::radio::ims::ImsTrafficType, 7> enum_values<aidl::android::hardware::radio::ims::ImsTrafficType> = {
  aidl::android::hardware::radio::ims::ImsTrafficType::EMERGENCY,
  aidl::android::hardware::radio::ims::ImsTrafficType::EMERGENCY_SMS,
  aidl::android::hardware::radio::ims::ImsTrafficType::VOICE,
  aidl::android::hardware::radio::ims::ImsTrafficType::VIDEO,
  aidl::android::hardware::radio::ims::ImsTrafficType::SMS,
  aidl::android::hardware::radio::ims::ImsTrafficType::REGISTRATION,
  aidl::android::hardware::radio::ims::ImsTrafficType::UT_XCAP,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
