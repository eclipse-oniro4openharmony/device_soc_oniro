#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_enums.h>
#include <android/binder_interface_utils.h>
#include <android/binder_parcelable_utils.h>
#include <android/binder_to_string.h>
#include <aidl/android/hardware/radio/network/EutranRegistrationInfo.h>
#include <aidl/android/hardware/radio/network/LteVopsInfo.h>
#include <aidl/android/hardware/radio/network/NrIndicators.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::radio::network {
class LteVopsInfo;
class NrIndicators;
}  // namespace aidl::android::hardware::radio::network
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace network {
class EutranRegistrationInfo {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  enum class AttachResultType : int8_t {
    NONE = 0,
    EPS_ONLY = 1,
    COMBINED = 2,
  };

  ::aidl::android::hardware::radio::network::LteVopsInfo lteVopsInfo;
  ::aidl::android::hardware::radio::network::NrIndicators nrIndicators;
  ::aidl::android::hardware::radio::network::EutranRegistrationInfo::AttachResultType lteAttachResultType = ::aidl::android::hardware::radio::network::EutranRegistrationInfo::AttachResultType(0);
  int32_t extraInfo = 0;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator!=(const EutranRegistrationInfo& rhs) const {
    return std::tie(lteVopsInfo, nrIndicators, lteAttachResultType, extraInfo) != std::tie(rhs.lteVopsInfo, rhs.nrIndicators, rhs.lteAttachResultType, rhs.extraInfo);
  }
  inline bool operator<(const EutranRegistrationInfo& rhs) const {
    return std::tie(lteVopsInfo, nrIndicators, lteAttachResultType, extraInfo) < std::tie(rhs.lteVopsInfo, rhs.nrIndicators, rhs.lteAttachResultType, rhs.extraInfo);
  }
  inline bool operator<=(const EutranRegistrationInfo& rhs) const {
    return std::tie(lteVopsInfo, nrIndicators, lteAttachResultType, extraInfo) <= std::tie(rhs.lteVopsInfo, rhs.nrIndicators, rhs.lteAttachResultType, rhs.extraInfo);
  }
  inline bool operator==(const EutranRegistrationInfo& rhs) const {
    return std::tie(lteVopsInfo, nrIndicators, lteAttachResultType, extraInfo) == std::tie(rhs.lteVopsInfo, rhs.nrIndicators, rhs.lteAttachResultType, rhs.extraInfo);
  }
  inline bool operator>(const EutranRegistrationInfo& rhs) const {
    return std::tie(lteVopsInfo, nrIndicators, lteAttachResultType, extraInfo) > std::tie(rhs.lteVopsInfo, rhs.nrIndicators, rhs.lteAttachResultType, rhs.extraInfo);
  }
  inline bool operator>=(const EutranRegistrationInfo& rhs) const {
    return std::tie(lteVopsInfo, nrIndicators, lteAttachResultType, extraInfo) >= std::tie(rhs.lteVopsInfo, rhs.nrIndicators, rhs.lteAttachResultType, rhs.extraInfo);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  enum : int32_t { EXTRA_CSFB_NOT_PREFERRED = 1 };
  enum : int32_t { EXTRA_SMS_ONLY = 2 };
  inline std::string toString() const {
    std::ostringstream os;
    os << "EutranRegistrationInfo{";
    os << "lteVopsInfo: " << ::android::internal::ToString(lteVopsInfo);
    os << ", nrIndicators: " << ::android::internal::ToString(nrIndicators);
    os << ", lteAttachResultType: " << ::android::internal::ToString(lteAttachResultType);
    os << ", extraInfo: " << ::android::internal::ToString(extraInfo);
    os << "}";
    return os.str();
  }
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
[[nodiscard]] static inline std::string toString(EutranRegistrationInfo::AttachResultType val) {
  switch(val) {
  case EutranRegistrationInfo::AttachResultType::NONE:
    return "NONE";
  case EutranRegistrationInfo::AttachResultType::EPS_ONLY:
    return "EPS_ONLY";
  case EutranRegistrationInfo::AttachResultType::COMBINED:
    return "COMBINED";
  default:
    return std::to_string(static_cast<int8_t>(val));
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
constexpr inline std::array<aidl::android::hardware::radio::network::EutranRegistrationInfo::AttachResultType, 3> enum_values<aidl::android::hardware::radio::network::EutranRegistrationInfo::AttachResultType> = {
  aidl::android::hardware::radio::network::EutranRegistrationInfo::AttachResultType::NONE,
  aidl::android::hardware::radio::network::EutranRegistrationInfo::AttachResultType::EPS_ONLY,
  aidl::android::hardware::radio::network::EutranRegistrationInfo::AttachResultType::COMBINED,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
