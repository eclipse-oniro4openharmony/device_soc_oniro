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
#include <aidl/android/hardware/radio/ims/ImsRegistrationState.h>
#include <aidl/android/hardware/radio/ims/SuggestedAction.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class ImsRegistration {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  ::aidl::android::hardware::radio::ims::ImsRegistrationState regState = ::aidl::android::hardware::radio::ims::ImsRegistrationState(0);
  ::aidl::android::hardware::radio::AccessNetwork accessNetworkType = ::aidl::android::hardware::radio::AccessNetwork(0);
  ::aidl::android::hardware::radio::ims::SuggestedAction suggestedAction = ::aidl::android::hardware::radio::ims::SuggestedAction(0);
  int32_t capabilities = 0;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator!=(const ImsRegistration& rhs) const {
    return std::tie(regState, accessNetworkType, suggestedAction, capabilities) != std::tie(rhs.regState, rhs.accessNetworkType, rhs.suggestedAction, rhs.capabilities);
  }
  inline bool operator<(const ImsRegistration& rhs) const {
    return std::tie(regState, accessNetworkType, suggestedAction, capabilities) < std::tie(rhs.regState, rhs.accessNetworkType, rhs.suggestedAction, rhs.capabilities);
  }
  inline bool operator<=(const ImsRegistration& rhs) const {
    return std::tie(regState, accessNetworkType, suggestedAction, capabilities) <= std::tie(rhs.regState, rhs.accessNetworkType, rhs.suggestedAction, rhs.capabilities);
  }
  inline bool operator==(const ImsRegistration& rhs) const {
    return std::tie(regState, accessNetworkType, suggestedAction, capabilities) == std::tie(rhs.regState, rhs.accessNetworkType, rhs.suggestedAction, rhs.capabilities);
  }
  inline bool operator>(const ImsRegistration& rhs) const {
    return std::tie(regState, accessNetworkType, suggestedAction, capabilities) > std::tie(rhs.regState, rhs.accessNetworkType, rhs.suggestedAction, rhs.capabilities);
  }
  inline bool operator>=(const ImsRegistration& rhs) const {
    return std::tie(regState, accessNetworkType, suggestedAction, capabilities) >= std::tie(rhs.regState, rhs.accessNetworkType, rhs.suggestedAction, rhs.capabilities);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  enum : int32_t { IMS_MMTEL_CAPABILITY_NONE = 0 };
  enum : int32_t { IMS_MMTEL_CAPABILITY_VOICE = 1 };
  enum : int32_t { IMS_MMTEL_CAPABILITY_VIDEO = 2 };
  enum : int32_t { IMS_MMTEL_CAPABILITY_SMS = 4 };
  enum : int32_t { IMS_RCS_CAPABILITIES = 8 };
  inline std::string toString() const {
    std::ostringstream os;
    os << "ImsRegistration{";
    os << "regState: " << ::android::internal::ToString(regState);
    os << ", accessNetworkType: " << ::android::internal::ToString(accessNetworkType);
    os << ", suggestedAction: " << ::android::internal::ToString(suggestedAction);
    os << ", capabilities: " << ::android::internal::ToString(capabilities);
    os << "}";
    return os.str();
  }
};
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
