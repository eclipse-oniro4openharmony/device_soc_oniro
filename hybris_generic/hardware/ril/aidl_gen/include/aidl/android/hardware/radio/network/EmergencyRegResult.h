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
#include <aidl/android/hardware/radio/network/Domain.h>
#include <aidl/android/hardware/radio/network/RegState.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace network {
class EmergencyRegResult {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  ::aidl::android::hardware::radio::AccessNetwork accessNetwork = ::aidl::android::hardware::radio::AccessNetwork(0);
  ::aidl::android::hardware::radio::network::RegState regState = ::aidl::android::hardware::radio::network::RegState(0);
  ::aidl::android::hardware::radio::network::Domain emcDomain = ::aidl::android::hardware::radio::network::Domain(0);
  bool isVopsSupported = false;
  bool isEmcBearerSupported = false;
  int8_t nwProvidedEmc = 0;
  int8_t nwProvidedEmf = 0;
  std::string mcc = "";
  std::string mnc = "";

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator!=(const EmergencyRegResult& rhs) const {
    return std::tie(accessNetwork, regState, emcDomain, isVopsSupported, isEmcBearerSupported, nwProvidedEmc, nwProvidedEmf, mcc, mnc) != std::tie(rhs.accessNetwork, rhs.regState, rhs.emcDomain, rhs.isVopsSupported, rhs.isEmcBearerSupported, rhs.nwProvidedEmc, rhs.nwProvidedEmf, rhs.mcc, rhs.mnc);
  }
  inline bool operator<(const EmergencyRegResult& rhs) const {
    return std::tie(accessNetwork, regState, emcDomain, isVopsSupported, isEmcBearerSupported, nwProvidedEmc, nwProvidedEmf, mcc, mnc) < std::tie(rhs.accessNetwork, rhs.regState, rhs.emcDomain, rhs.isVopsSupported, rhs.isEmcBearerSupported, rhs.nwProvidedEmc, rhs.nwProvidedEmf, rhs.mcc, rhs.mnc);
  }
  inline bool operator<=(const EmergencyRegResult& rhs) const {
    return std::tie(accessNetwork, regState, emcDomain, isVopsSupported, isEmcBearerSupported, nwProvidedEmc, nwProvidedEmf, mcc, mnc) <= std::tie(rhs.accessNetwork, rhs.regState, rhs.emcDomain, rhs.isVopsSupported, rhs.isEmcBearerSupported, rhs.nwProvidedEmc, rhs.nwProvidedEmf, rhs.mcc, rhs.mnc);
  }
  inline bool operator==(const EmergencyRegResult& rhs) const {
    return std::tie(accessNetwork, regState, emcDomain, isVopsSupported, isEmcBearerSupported, nwProvidedEmc, nwProvidedEmf, mcc, mnc) == std::tie(rhs.accessNetwork, rhs.regState, rhs.emcDomain, rhs.isVopsSupported, rhs.isEmcBearerSupported, rhs.nwProvidedEmc, rhs.nwProvidedEmf, rhs.mcc, rhs.mnc);
  }
  inline bool operator>(const EmergencyRegResult& rhs) const {
    return std::tie(accessNetwork, regState, emcDomain, isVopsSupported, isEmcBearerSupported, nwProvidedEmc, nwProvidedEmf, mcc, mnc) > std::tie(rhs.accessNetwork, rhs.regState, rhs.emcDomain, rhs.isVopsSupported, rhs.isEmcBearerSupported, rhs.nwProvidedEmc, rhs.nwProvidedEmf, rhs.mcc, rhs.mnc);
  }
  inline bool operator>=(const EmergencyRegResult& rhs) const {
    return std::tie(accessNetwork, regState, emcDomain, isVopsSupported, isEmcBearerSupported, nwProvidedEmc, nwProvidedEmf, mcc, mnc) >= std::tie(rhs.accessNetwork, rhs.regState, rhs.emcDomain, rhs.isVopsSupported, rhs.isEmcBearerSupported, rhs.nwProvidedEmc, rhs.nwProvidedEmf, rhs.mcc, rhs.mnc);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream os;
    os << "EmergencyRegResult{";
    os << "accessNetwork: " << ::android::internal::ToString(accessNetwork);
    os << ", regState: " << ::android::internal::ToString(regState);
    os << ", emcDomain: " << ::android::internal::ToString(emcDomain);
    os << ", isVopsSupported: " << ::android::internal::ToString(isVopsSupported);
    os << ", isEmcBearerSupported: " << ::android::internal::ToString(isEmcBearerSupported);
    os << ", nwProvidedEmc: " << ::android::internal::ToString(nwProvidedEmc);
    os << ", nwProvidedEmf: " << ::android::internal::ToString(nwProvidedEmf);
    os << ", mcc: " << ::android::internal::ToString(mcc);
    os << ", mnc: " << ::android::internal::ToString(mnc);
    os << "}";
    return os.str();
  }
};
}  // namespace network
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
