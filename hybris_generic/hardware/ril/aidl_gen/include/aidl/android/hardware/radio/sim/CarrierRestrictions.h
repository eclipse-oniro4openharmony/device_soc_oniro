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
#include <aidl/android/hardware/radio/sim/Carrier.h>
#include <aidl/android/hardware/radio/sim/CarrierRestrictions.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::radio::sim {
class Carrier;
}  // namespace aidl::android::hardware::radio::sim
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace sim {
class CarrierRestrictions {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  enum class CarrierRestrictionStatus : int32_t {
    UNKNOWN = 0,
    NOT_RESTRICTED = 1,
    RESTRICTED = 2,
  };

  std::vector<::aidl::android::hardware::radio::sim::Carrier> allowedCarriers;
  std::vector<::aidl::android::hardware::radio::sim::Carrier> excludedCarriers;
  bool allowedCarriersPrioritized = false;
  ::aidl::android::hardware::radio::sim::CarrierRestrictions::CarrierRestrictionStatus status = ::aidl::android::hardware::radio::sim::CarrierRestrictions::CarrierRestrictionStatus(0);

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator!=(const CarrierRestrictions& rhs) const {
    return std::tie(allowedCarriers, excludedCarriers, allowedCarriersPrioritized, status) != std::tie(rhs.allowedCarriers, rhs.excludedCarriers, rhs.allowedCarriersPrioritized, rhs.status);
  }
  inline bool operator<(const CarrierRestrictions& rhs) const {
    return std::tie(allowedCarriers, excludedCarriers, allowedCarriersPrioritized, status) < std::tie(rhs.allowedCarriers, rhs.excludedCarriers, rhs.allowedCarriersPrioritized, rhs.status);
  }
  inline bool operator<=(const CarrierRestrictions& rhs) const {
    return std::tie(allowedCarriers, excludedCarriers, allowedCarriersPrioritized, status) <= std::tie(rhs.allowedCarriers, rhs.excludedCarriers, rhs.allowedCarriersPrioritized, rhs.status);
  }
  inline bool operator==(const CarrierRestrictions& rhs) const {
    return std::tie(allowedCarriers, excludedCarriers, allowedCarriersPrioritized, status) == std::tie(rhs.allowedCarriers, rhs.excludedCarriers, rhs.allowedCarriersPrioritized, rhs.status);
  }
  inline bool operator>(const CarrierRestrictions& rhs) const {
    return std::tie(allowedCarriers, excludedCarriers, allowedCarriersPrioritized, status) > std::tie(rhs.allowedCarriers, rhs.excludedCarriers, rhs.allowedCarriersPrioritized, rhs.status);
  }
  inline bool operator>=(const CarrierRestrictions& rhs) const {
    return std::tie(allowedCarriers, excludedCarriers, allowedCarriersPrioritized, status) >= std::tie(rhs.allowedCarriers, rhs.excludedCarriers, rhs.allowedCarriersPrioritized, rhs.status);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream os;
    os << "CarrierRestrictions{";
    os << "allowedCarriers: " << ::android::internal::ToString(allowedCarriers);
    os << ", excludedCarriers: " << ::android::internal::ToString(excludedCarriers);
    os << ", allowedCarriersPrioritized: " << ::android::internal::ToString(allowedCarriersPrioritized);
    os << ", status: " << ::android::internal::ToString(status);
    os << "}";
    return os.str();
  }
};
}  // namespace sim
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace sim {
[[nodiscard]] static inline std::string toString(CarrierRestrictions::CarrierRestrictionStatus val) {
  switch(val) {
  case CarrierRestrictions::CarrierRestrictionStatus::UNKNOWN:
    return "UNKNOWN";
  case CarrierRestrictions::CarrierRestrictionStatus::NOT_RESTRICTED:
    return "NOT_RESTRICTED";
  case CarrierRestrictions::CarrierRestrictionStatus::RESTRICTED:
    return "RESTRICTED";
  default:
    return std::to_string(static_cast<int32_t>(val));
  }
}
}  // namespace sim
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace ndk {
namespace internal {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"
template <>
constexpr inline std::array<aidl::android::hardware::radio::sim::CarrierRestrictions::CarrierRestrictionStatus, 3> enum_values<aidl::android::hardware::radio::sim::CarrierRestrictions::CarrierRestrictionStatus> = {
  aidl::android::hardware::radio::sim::CarrierRestrictions::CarrierRestrictionStatus::UNKNOWN,
  aidl::android::hardware::radio::sim::CarrierRestrictions::CarrierRestrictionStatus::NOT_RESTRICTED,
  aidl::android::hardware::radio::sim::CarrierRestrictions::CarrierRestrictionStatus::RESTRICTED,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
