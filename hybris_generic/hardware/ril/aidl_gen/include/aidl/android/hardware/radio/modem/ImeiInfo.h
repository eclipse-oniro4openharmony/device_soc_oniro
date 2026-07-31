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
#include <aidl/android/hardware/radio/modem/ImeiInfo.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace modem {
class ImeiInfo {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  enum class ImeiType : int32_t {
    PRIMARY = 1,
    SECONDARY = 2,
  };

  ::aidl::android::hardware::radio::modem::ImeiInfo::ImeiType type = ::aidl::android::hardware::radio::modem::ImeiInfo::ImeiType(0);
  std::string imei;
  std::string svn;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator!=(const ImeiInfo& rhs) const {
    return std::tie(type, imei, svn) != std::tie(rhs.type, rhs.imei, rhs.svn);
  }
  inline bool operator<(const ImeiInfo& rhs) const {
    return std::tie(type, imei, svn) < std::tie(rhs.type, rhs.imei, rhs.svn);
  }
  inline bool operator<=(const ImeiInfo& rhs) const {
    return std::tie(type, imei, svn) <= std::tie(rhs.type, rhs.imei, rhs.svn);
  }
  inline bool operator==(const ImeiInfo& rhs) const {
    return std::tie(type, imei, svn) == std::tie(rhs.type, rhs.imei, rhs.svn);
  }
  inline bool operator>(const ImeiInfo& rhs) const {
    return std::tie(type, imei, svn) > std::tie(rhs.type, rhs.imei, rhs.svn);
  }
  inline bool operator>=(const ImeiInfo& rhs) const {
    return std::tie(type, imei, svn) >= std::tie(rhs.type, rhs.imei, rhs.svn);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream os;
    os << "ImeiInfo{";
    os << "type: " << ::android::internal::ToString(type);
    os << ", imei: " << ::android::internal::ToString(imei);
    os << ", svn: " << ::android::internal::ToString(svn);
    os << "}";
    return os.str();
  }
};
}  // namespace modem
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace modem {
[[nodiscard]] static inline std::string toString(ImeiInfo::ImeiType val) {
  switch(val) {
  case ImeiInfo::ImeiType::PRIMARY:
    return "PRIMARY";
  case ImeiInfo::ImeiType::SECONDARY:
    return "SECONDARY";
  default:
    return std::to_string(static_cast<int32_t>(val));
  }
}
}  // namespace modem
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
namespace ndk {
namespace internal {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++17-extensions"
template <>
constexpr inline std::array<aidl::android::hardware::radio::modem::ImeiInfo::ImeiType, 2> enum_values<aidl::android::hardware::radio::modem::ImeiInfo::ImeiType> = {
  aidl::android::hardware::radio::modem::ImeiInfo::ImeiType::PRIMARY,
  aidl::android::hardware::radio::modem::ImeiInfo::ImeiType::SECONDARY,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
