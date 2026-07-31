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
enum class ImsRegistrationState : int32_t {
  NOT_REGISTERED = 0,
  REGISTERED = 1,
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
[[nodiscard]] static inline std::string toString(ImsRegistrationState val) {
  switch(val) {
  case ImsRegistrationState::NOT_REGISTERED:
    return "NOT_REGISTERED";
  case ImsRegistrationState::REGISTERED:
    return "REGISTERED";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::ImsRegistrationState, 2> enum_values<aidl::android::hardware::radio::ims::ImsRegistrationState> = {
  aidl::android::hardware::radio::ims::ImsRegistrationState::NOT_REGISTERED,
  aidl::android::hardware::radio::ims::ImsRegistrationState::REGISTERED,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
