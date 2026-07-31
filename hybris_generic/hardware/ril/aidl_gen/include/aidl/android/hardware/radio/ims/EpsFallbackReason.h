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
enum class EpsFallbackReason : int32_t {
  NO_NETWORK_TRIGGER = 1,
  NO_NETWORK_RESPONSE = 2,
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
[[nodiscard]] static inline std::string toString(EpsFallbackReason val) {
  switch(val) {
  case EpsFallbackReason::NO_NETWORK_TRIGGER:
    return "NO_NETWORK_TRIGGER";
  case EpsFallbackReason::NO_NETWORK_RESPONSE:
    return "NO_NETWORK_RESPONSE";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::EpsFallbackReason, 2> enum_values<aidl::android::hardware::radio::ims::EpsFallbackReason> = {
  aidl::android::hardware::radio::ims::EpsFallbackReason::NO_NETWORK_TRIGGER,
  aidl::android::hardware::radio::ims::EpsFallbackReason::NO_NETWORK_RESPONSE,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
