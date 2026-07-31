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
enum class ImsDeregistrationReason : int32_t {
  REASON_SIM_REMOVED = 1,
  REASON_SIM_REFRESH = 2,
  REASON_ALLOWED_NETWORK_TYPES_CHANGED = 3,
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
[[nodiscard]] static inline std::string toString(ImsDeregistrationReason val) {
  switch(val) {
  case ImsDeregistrationReason::REASON_SIM_REMOVED:
    return "REASON_SIM_REMOVED";
  case ImsDeregistrationReason::REASON_SIM_REFRESH:
    return "REASON_SIM_REFRESH";
  case ImsDeregistrationReason::REASON_ALLOWED_NETWORK_TYPES_CHANGED:
    return "REASON_ALLOWED_NETWORK_TYPES_CHANGED";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::ImsDeregistrationReason, 3> enum_values<aidl::android::hardware::radio::ims::ImsDeregistrationReason> = {
  aidl::android::hardware::radio::ims::ImsDeregistrationReason::REASON_SIM_REMOVED,
  aidl::android::hardware::radio::ims::ImsDeregistrationReason::REASON_SIM_REFRESH,
  aidl::android::hardware::radio::ims::ImsDeregistrationReason::REASON_ALLOWED_NETWORK_TYPES_CHANGED,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
