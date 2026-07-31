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
enum class ImsStreamType : int32_t {
  AUDIO = 1,
  VIDEO = 2,
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
[[nodiscard]] static inline std::string toString(ImsStreamType val) {
  switch(val) {
  case ImsStreamType::AUDIO:
    return "AUDIO";
  case ImsStreamType::VIDEO:
    return "VIDEO";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::ImsStreamType, 2> enum_values<aidl::android::hardware::radio::ims::ImsStreamType> = {
  aidl::android::hardware::radio::ims::ImsStreamType::AUDIO,
  aidl::android::hardware::radio::ims::ImsStreamType::VIDEO,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
