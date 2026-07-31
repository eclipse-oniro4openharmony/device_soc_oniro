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
enum class SuggestedAction : int32_t {
  NONE = 0,
  TRIGGER_PLMN_BLOCK = 1,
  TRIGGER_PLMN_BLOCK_WITH_TIMEOUT = 2,
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
[[nodiscard]] static inline std::string toString(SuggestedAction val) {
  switch(val) {
  case SuggestedAction::NONE:
    return "NONE";
  case SuggestedAction::TRIGGER_PLMN_BLOCK:
    return "TRIGGER_PLMN_BLOCK";
  case SuggestedAction::TRIGGER_PLMN_BLOCK_WITH_TIMEOUT:
    return "TRIGGER_PLMN_BLOCK_WITH_TIMEOUT";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::SuggestedAction, 3> enum_values<aidl::android::hardware::radio::ims::SuggestedAction> = {
  aidl::android::hardware::radio::ims::SuggestedAction::NONE,
  aidl::android::hardware::radio::ims::SuggestedAction::TRIGGER_PLMN_BLOCK,
  aidl::android::hardware::radio::ims::SuggestedAction::TRIGGER_PLMN_BLOCK_WITH_TIMEOUT,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
