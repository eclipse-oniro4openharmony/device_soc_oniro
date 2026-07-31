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
#include <aidl/android/hardware/radio/ims/SrvccCall.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class SrvccCall {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  enum class CallType : int32_t {
    NORMAL = 0,
    EMERGENCY = 1,
  };

  enum class CallSubState : int32_t {
    NONE = 0,
    PREALERTING = 1,
  };

  enum class ToneType : int32_t {
    NONE = 0,
    LOCAL = 1,
    NETWORK = 2,
  };

  int32_t index = 0;
  ::aidl::android::hardware::radio::ims::SrvccCall::CallType callType = ::aidl::android::hardware::radio::ims::SrvccCall::CallType(0);
  int32_t callState = 0;
  ::aidl::android::hardware::radio::ims::SrvccCall::CallSubState callSubstate = ::aidl::android::hardware::radio::ims::SrvccCall::CallSubState(0);
  ::aidl::android::hardware::radio::ims::SrvccCall::ToneType ringbackToneType = ::aidl::android::hardware::radio::ims::SrvccCall::ToneType(0);
  bool isMpty = false;
  bool isMT = false;
  std::string number;
  int32_t numPresentation = 0;
  std::string name;
  int32_t namePresentation = 0;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator!=(const SrvccCall& rhs) const {
    return std::tie(index, callType, callState, callSubstate, ringbackToneType, isMpty, isMT, number, numPresentation, name, namePresentation) != std::tie(rhs.index, rhs.callType, rhs.callState, rhs.callSubstate, rhs.ringbackToneType, rhs.isMpty, rhs.isMT, rhs.number, rhs.numPresentation, rhs.name, rhs.namePresentation);
  }
  inline bool operator<(const SrvccCall& rhs) const {
    return std::tie(index, callType, callState, callSubstate, ringbackToneType, isMpty, isMT, number, numPresentation, name, namePresentation) < std::tie(rhs.index, rhs.callType, rhs.callState, rhs.callSubstate, rhs.ringbackToneType, rhs.isMpty, rhs.isMT, rhs.number, rhs.numPresentation, rhs.name, rhs.namePresentation);
  }
  inline bool operator<=(const SrvccCall& rhs) const {
    return std::tie(index, callType, callState, callSubstate, ringbackToneType, isMpty, isMT, number, numPresentation, name, namePresentation) <= std::tie(rhs.index, rhs.callType, rhs.callState, rhs.callSubstate, rhs.ringbackToneType, rhs.isMpty, rhs.isMT, rhs.number, rhs.numPresentation, rhs.name, rhs.namePresentation);
  }
  inline bool operator==(const SrvccCall& rhs) const {
    return std::tie(index, callType, callState, callSubstate, ringbackToneType, isMpty, isMT, number, numPresentation, name, namePresentation) == std::tie(rhs.index, rhs.callType, rhs.callState, rhs.callSubstate, rhs.ringbackToneType, rhs.isMpty, rhs.isMT, rhs.number, rhs.numPresentation, rhs.name, rhs.namePresentation);
  }
  inline bool operator>(const SrvccCall& rhs) const {
    return std::tie(index, callType, callState, callSubstate, ringbackToneType, isMpty, isMT, number, numPresentation, name, namePresentation) > std::tie(rhs.index, rhs.callType, rhs.callState, rhs.callSubstate, rhs.ringbackToneType, rhs.isMpty, rhs.isMT, rhs.number, rhs.numPresentation, rhs.name, rhs.namePresentation);
  }
  inline bool operator>=(const SrvccCall& rhs) const {
    return std::tie(index, callType, callState, callSubstate, ringbackToneType, isMpty, isMT, number, numPresentation, name, namePresentation) >= std::tie(rhs.index, rhs.callType, rhs.callState, rhs.callSubstate, rhs.ringbackToneType, rhs.isMpty, rhs.isMT, rhs.number, rhs.numPresentation, rhs.name, rhs.namePresentation);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream os;
    os << "SrvccCall{";
    os << "index: " << ::android::internal::ToString(index);
    os << ", callType: " << ::android::internal::ToString(callType);
    os << ", callState: " << ::android::internal::ToString(callState);
    os << ", callSubstate: " << ::android::internal::ToString(callSubstate);
    os << ", ringbackToneType: " << ::android::internal::ToString(ringbackToneType);
    os << ", isMpty: " << ::android::internal::ToString(isMpty);
    os << ", isMT: " << ::android::internal::ToString(isMT);
    os << ", number: " << ::android::internal::ToString(number);
    os << ", numPresentation: " << ::android::internal::ToString(numPresentation);
    os << ", name: " << ::android::internal::ToString(name);
    os << ", namePresentation: " << ::android::internal::ToString(namePresentation);
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
[[nodiscard]] static inline std::string toString(SrvccCall::CallType val) {
  switch(val) {
  case SrvccCall::CallType::NORMAL:
    return "NORMAL";
  case SrvccCall::CallType::EMERGENCY:
    return "EMERGENCY";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::SrvccCall::CallType, 2> enum_values<aidl::android::hardware::radio::ims::SrvccCall::CallType> = {
  aidl::android::hardware::radio::ims::SrvccCall::CallType::NORMAL,
  aidl::android::hardware::radio::ims::SrvccCall::CallType::EMERGENCY,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
[[nodiscard]] static inline std::string toString(SrvccCall::CallSubState val) {
  switch(val) {
  case SrvccCall::CallSubState::NONE:
    return "NONE";
  case SrvccCall::CallSubState::PREALERTING:
    return "PREALERTING";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::SrvccCall::CallSubState, 2> enum_values<aidl::android::hardware::radio::ims::SrvccCall::CallSubState> = {
  aidl::android::hardware::radio::ims::SrvccCall::CallSubState::NONE,
  aidl::android::hardware::radio::ims::SrvccCall::CallSubState::PREALERTING,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
[[nodiscard]] static inline std::string toString(SrvccCall::ToneType val) {
  switch(val) {
  case SrvccCall::ToneType::NONE:
    return "NONE";
  case SrvccCall::ToneType::LOCAL:
    return "LOCAL";
  case SrvccCall::ToneType::NETWORK:
    return "NETWORK";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::SrvccCall::ToneType, 3> enum_values<aidl::android::hardware::radio::ims::SrvccCall::ToneType> = {
  aidl::android::hardware::radio::ims::SrvccCall::ToneType::NONE,
  aidl::android::hardware::radio::ims::SrvccCall::ToneType::LOCAL,
  aidl::android::hardware::radio::ims::SrvccCall::ToneType::NETWORK,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
