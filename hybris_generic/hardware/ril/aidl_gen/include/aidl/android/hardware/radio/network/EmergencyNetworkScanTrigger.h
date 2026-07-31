#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_interface_utils.h>
#include <android/binder_parcelable_utils.h>
#include <android/binder_to_string.h>
#include <aidl/android/hardware/radio/AccessNetwork.h>
#include <aidl/android/hardware/radio/network/EmergencyScanType.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace network {
class EmergencyNetworkScanTrigger {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  std::vector<::aidl::android::hardware::radio::AccessNetwork> accessNetwork;
  ::aidl::android::hardware::radio::network::EmergencyScanType scanType = ::aidl::android::hardware::radio::network::EmergencyScanType(0);

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator!=(const EmergencyNetworkScanTrigger& rhs) const {
    return std::tie(accessNetwork, scanType) != std::tie(rhs.accessNetwork, rhs.scanType);
  }
  inline bool operator<(const EmergencyNetworkScanTrigger& rhs) const {
    return std::tie(accessNetwork, scanType) < std::tie(rhs.accessNetwork, rhs.scanType);
  }
  inline bool operator<=(const EmergencyNetworkScanTrigger& rhs) const {
    return std::tie(accessNetwork, scanType) <= std::tie(rhs.accessNetwork, rhs.scanType);
  }
  inline bool operator==(const EmergencyNetworkScanTrigger& rhs) const {
    return std::tie(accessNetwork, scanType) == std::tie(rhs.accessNetwork, rhs.scanType);
  }
  inline bool operator>(const EmergencyNetworkScanTrigger& rhs) const {
    return std::tie(accessNetwork, scanType) > std::tie(rhs.accessNetwork, rhs.scanType);
  }
  inline bool operator>=(const EmergencyNetworkScanTrigger& rhs) const {
    return std::tie(accessNetwork, scanType) >= std::tie(rhs.accessNetwork, rhs.scanType);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream os;
    os << "EmergencyNetworkScanTrigger{";
    os << "accessNetwork: " << ::android::internal::ToString(accessNetwork);
    os << ", scanType: " << ::android::internal::ToString(scanType);
    os << "}";
    return os.str();
  }
};
}  // namespace network
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
