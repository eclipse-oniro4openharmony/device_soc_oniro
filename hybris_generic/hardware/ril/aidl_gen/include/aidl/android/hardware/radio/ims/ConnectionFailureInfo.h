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
#include <aidl/android/hardware/radio/ims/ConnectionFailureInfo.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class ConnectionFailureInfo {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  enum class ConnectionFailureReason : int32_t {
    REASON_ACCESS_DENIED = 1,
    REASON_NAS_FAILURE = 2,
    REASON_RACH_FAILURE = 3,
    REASON_RLC_FAILURE = 4,
    REASON_RRC_REJECT = 5,
    REASON_RRC_TIMEOUT = 6,
    REASON_NO_SERVICE = 7,
    REASON_PDN_NOT_AVAILABLE = 8,
    REASON_RF_BUSY = 9,
    REASON_UNSPECIFIED = 65535,
  };

  ::aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason failureReason = ::aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason(0);
  int32_t causeCode = 0;
  int32_t waitTimeMillis = 0;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator!=(const ConnectionFailureInfo& rhs) const {
    return std::tie(failureReason, causeCode, waitTimeMillis) != std::tie(rhs.failureReason, rhs.causeCode, rhs.waitTimeMillis);
  }
  inline bool operator<(const ConnectionFailureInfo& rhs) const {
    return std::tie(failureReason, causeCode, waitTimeMillis) < std::tie(rhs.failureReason, rhs.causeCode, rhs.waitTimeMillis);
  }
  inline bool operator<=(const ConnectionFailureInfo& rhs) const {
    return std::tie(failureReason, causeCode, waitTimeMillis) <= std::tie(rhs.failureReason, rhs.causeCode, rhs.waitTimeMillis);
  }
  inline bool operator==(const ConnectionFailureInfo& rhs) const {
    return std::tie(failureReason, causeCode, waitTimeMillis) == std::tie(rhs.failureReason, rhs.causeCode, rhs.waitTimeMillis);
  }
  inline bool operator>(const ConnectionFailureInfo& rhs) const {
    return std::tie(failureReason, causeCode, waitTimeMillis) > std::tie(rhs.failureReason, rhs.causeCode, rhs.waitTimeMillis);
  }
  inline bool operator>=(const ConnectionFailureInfo& rhs) const {
    return std::tie(failureReason, causeCode, waitTimeMillis) >= std::tie(rhs.failureReason, rhs.causeCode, rhs.waitTimeMillis);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream os;
    os << "ConnectionFailureInfo{";
    os << "failureReason: " << ::android::internal::ToString(failureReason);
    os << ", causeCode: " << ::android::internal::ToString(causeCode);
    os << ", waitTimeMillis: " << ::android::internal::ToString(waitTimeMillis);
    os << "}";
    return os.str();
  }
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
[[nodiscard]] static inline std::string toString(ConnectionFailureInfo::ConnectionFailureReason val) {
  switch(val) {
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_ACCESS_DENIED:
    return "REASON_ACCESS_DENIED";
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_NAS_FAILURE:
    return "REASON_NAS_FAILURE";
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_RACH_FAILURE:
    return "REASON_RACH_FAILURE";
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_RLC_FAILURE:
    return "REASON_RLC_FAILURE";
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_RRC_REJECT:
    return "REASON_RRC_REJECT";
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_RRC_TIMEOUT:
    return "REASON_RRC_TIMEOUT";
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_NO_SERVICE:
    return "REASON_NO_SERVICE";
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_PDN_NOT_AVAILABLE:
    return "REASON_PDN_NOT_AVAILABLE";
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_RF_BUSY:
    return "REASON_RF_BUSY";
  case ConnectionFailureInfo::ConnectionFailureReason::REASON_UNSPECIFIED:
    return "REASON_UNSPECIFIED";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason, 10> enum_values<aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason> = {
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_ACCESS_DENIED,
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_NAS_FAILURE,
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_RACH_FAILURE,
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_RLC_FAILURE,
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_RRC_REJECT,
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_RRC_TIMEOUT,
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_NO_SERVICE,
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_PDN_NOT_AVAILABLE,
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_RF_BUSY,
  aidl::android::hardware::radio::ims::ConnectionFailureInfo::ConnectionFailureReason::REASON_UNSPECIFIED,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
