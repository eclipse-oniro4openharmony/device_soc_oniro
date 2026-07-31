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
#include <aidl/android/hardware/radio/AccessNetwork.h>
#include <aidl/android/hardware/radio/ims/ImsCall.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class ImsCall {
public:
  typedef std::false_type fixed_size;
  static const char* descriptor;

  enum class CallType : int32_t {
    NORMAL = 0,
    EMERGENCY = 1,
  };

  enum class CallState : int32_t {
    ACTIVE = 0,
    HOLDING = 1,
    DIALING = 2,
    ALERTING = 3,
    INCOMING = 4,
    WAITING = 5,
    DISCONNECTING = 6,
    DISCONNECTED = 7,
  };

  enum class Direction : int32_t {
    INCOMING = 0,
    OUTGOING = 1,
  };

  int32_t index = 0;
  ::aidl::android::hardware::radio::ims::ImsCall::CallType callType = ::aidl::android::hardware::radio::ims::ImsCall::CallType(0);
  ::aidl::android::hardware::radio::AccessNetwork accessNetwork = ::aidl::android::hardware::radio::AccessNetwork(0);
  ::aidl::android::hardware::radio::ims::ImsCall::CallState callState = ::aidl::android::hardware::radio::ims::ImsCall::CallState(0);
  ::aidl::android::hardware::radio::ims::ImsCall::Direction direction = ::aidl::android::hardware::radio::ims::ImsCall::Direction(0);
  bool isHeldByRemote = false;

  binder_status_t readFromParcel(const AParcel* parcel);
  binder_status_t writeToParcel(AParcel* parcel) const;

  inline bool operator!=(const ImsCall& rhs) const {
    return std::tie(index, callType, accessNetwork, callState, direction, isHeldByRemote) != std::tie(rhs.index, rhs.callType, rhs.accessNetwork, rhs.callState, rhs.direction, rhs.isHeldByRemote);
  }
  inline bool operator<(const ImsCall& rhs) const {
    return std::tie(index, callType, accessNetwork, callState, direction, isHeldByRemote) < std::tie(rhs.index, rhs.callType, rhs.accessNetwork, rhs.callState, rhs.direction, rhs.isHeldByRemote);
  }
  inline bool operator<=(const ImsCall& rhs) const {
    return std::tie(index, callType, accessNetwork, callState, direction, isHeldByRemote) <= std::tie(rhs.index, rhs.callType, rhs.accessNetwork, rhs.callState, rhs.direction, rhs.isHeldByRemote);
  }
  inline bool operator==(const ImsCall& rhs) const {
    return std::tie(index, callType, accessNetwork, callState, direction, isHeldByRemote) == std::tie(rhs.index, rhs.callType, rhs.accessNetwork, rhs.callState, rhs.direction, rhs.isHeldByRemote);
  }
  inline bool operator>(const ImsCall& rhs) const {
    return std::tie(index, callType, accessNetwork, callState, direction, isHeldByRemote) > std::tie(rhs.index, rhs.callType, rhs.accessNetwork, rhs.callState, rhs.direction, rhs.isHeldByRemote);
  }
  inline bool operator>=(const ImsCall& rhs) const {
    return std::tie(index, callType, accessNetwork, callState, direction, isHeldByRemote) >= std::tie(rhs.index, rhs.callType, rhs.accessNetwork, rhs.callState, rhs.direction, rhs.isHeldByRemote);
  }

  static const ::ndk::parcelable_stability_t _aidl_stability = ::ndk::STABILITY_VINTF;
  inline std::string toString() const {
    std::ostringstream os;
    os << "ImsCall{";
    os << "index: " << ::android::internal::ToString(index);
    os << ", callType: " << ::android::internal::ToString(callType);
    os << ", accessNetwork: " << ::android::internal::ToString(accessNetwork);
    os << ", callState: " << ::android::internal::ToString(callState);
    os << ", direction: " << ::android::internal::ToString(direction);
    os << ", isHeldByRemote: " << ::android::internal::ToString(isHeldByRemote);
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
[[nodiscard]] static inline std::string toString(ImsCall::CallType val) {
  switch(val) {
  case ImsCall::CallType::NORMAL:
    return "NORMAL";
  case ImsCall::CallType::EMERGENCY:
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
constexpr inline std::array<aidl::android::hardware::radio::ims::ImsCall::CallType, 2> enum_values<aidl::android::hardware::radio::ims::ImsCall::CallType> = {
  aidl::android::hardware::radio::ims::ImsCall::CallType::NORMAL,
  aidl::android::hardware::radio::ims::ImsCall::CallType::EMERGENCY,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
[[nodiscard]] static inline std::string toString(ImsCall::CallState val) {
  switch(val) {
  case ImsCall::CallState::ACTIVE:
    return "ACTIVE";
  case ImsCall::CallState::HOLDING:
    return "HOLDING";
  case ImsCall::CallState::DIALING:
    return "DIALING";
  case ImsCall::CallState::ALERTING:
    return "ALERTING";
  case ImsCall::CallState::INCOMING:
    return "INCOMING";
  case ImsCall::CallState::WAITING:
    return "WAITING";
  case ImsCall::CallState::DISCONNECTING:
    return "DISCONNECTING";
  case ImsCall::CallState::DISCONNECTED:
    return "DISCONNECTED";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::ImsCall::CallState, 8> enum_values<aidl::android::hardware::radio::ims::ImsCall::CallState> = {
  aidl::android::hardware::radio::ims::ImsCall::CallState::ACTIVE,
  aidl::android::hardware::radio::ims::ImsCall::CallState::HOLDING,
  aidl::android::hardware::radio::ims::ImsCall::CallState::DIALING,
  aidl::android::hardware::radio::ims::ImsCall::CallState::ALERTING,
  aidl::android::hardware::radio::ims::ImsCall::CallState::INCOMING,
  aidl::android::hardware::radio::ims::ImsCall::CallState::WAITING,
  aidl::android::hardware::radio::ims::ImsCall::CallState::DISCONNECTING,
  aidl::android::hardware::radio::ims::ImsCall::CallState::DISCONNECTED,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
[[nodiscard]] static inline std::string toString(ImsCall::Direction val) {
  switch(val) {
  case ImsCall::Direction::INCOMING:
    return "INCOMING";
  case ImsCall::Direction::OUTGOING:
    return "OUTGOING";
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
constexpr inline std::array<aidl::android::hardware::radio::ims::ImsCall::Direction, 2> enum_values<aidl::android::hardware::radio::ims::ImsCall::Direction> = {
  aidl::android::hardware::radio::ims::ImsCall::Direction::INCOMING,
  aidl::android::hardware::radio::ims::ImsCall::Direction::OUTGOING,
};
#pragma clang diagnostic pop
}  // namespace internal
}  // namespace ndk
