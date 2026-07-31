#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_interface_utils.h>
#include <aidl/android/hardware/radio/AccessNetwork.h>
#include <aidl/android/hardware/radio/ims/EpsFallbackReason.h>
#include <aidl/android/hardware/radio/ims/IRadioImsIndication.h>
#include <aidl/android/hardware/radio/ims/IRadioImsResponse.h>
#include <aidl/android/hardware/radio/ims/ImsCall.h>
#include <aidl/android/hardware/radio/ims/ImsRegistration.h>
#include <aidl/android/hardware/radio/ims/ImsStreamDirection.h>
#include <aidl/android/hardware/radio/ims/ImsStreamType.h>
#include <aidl/android/hardware/radio/ims/ImsTrafficType.h>
#include <aidl/android/hardware/radio/ims/SrvccCall.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::radio::ims {
class IRadioImsIndication;
class IRadioImsResponse;
class ImsCall;
class ImsRegistration;
class SrvccCall;
}  // namespace aidl::android::hardware::radio::ims
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class IRadioImsDelegator;

class IRadioIms : public ::ndk::ICInterface {
public:
  typedef IRadioImsDelegator DefaultDelegator;
  static const char* descriptor;
  IRadioIms();
  virtual ~IRadioIms();

  static const int32_t version = 1;
  static constexpr uint32_t TRANSACTION_setSrvccCallInfo = FIRST_CALL_TRANSACTION + 0;
  static constexpr uint32_t TRANSACTION_updateImsRegistrationInfo = FIRST_CALL_TRANSACTION + 1;
  static constexpr uint32_t TRANSACTION_startImsTraffic = FIRST_CALL_TRANSACTION + 2;
  static constexpr uint32_t TRANSACTION_stopImsTraffic = FIRST_CALL_TRANSACTION + 3;
  static constexpr uint32_t TRANSACTION_triggerEpsFallback = FIRST_CALL_TRANSACTION + 4;
  static constexpr uint32_t TRANSACTION_setResponseFunctions = FIRST_CALL_TRANSACTION + 5;
  static constexpr uint32_t TRANSACTION_sendAnbrQuery = FIRST_CALL_TRANSACTION + 6;
  static constexpr uint32_t TRANSACTION_updateImsCallStatus = FIRST_CALL_TRANSACTION + 7;

  static std::shared_ptr<IRadioIms> fromBinder(const ::ndk::SpAIBinder& binder);
  static binder_status_t writeToParcel(AParcel* parcel, const std::shared_ptr<IRadioIms>& instance);
  static binder_status_t readFromParcel(const AParcel* parcel, std::shared_ptr<IRadioIms>* instance);
  static bool setDefaultImpl(const std::shared_ptr<IRadioIms>& impl);
  static const std::shared_ptr<IRadioIms>& getDefaultImpl();
  virtual ::ndk::ScopedAStatus setSrvccCallInfo(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::SrvccCall>& in_srvccCalls) = 0;
  virtual ::ndk::ScopedAStatus updateImsRegistrationInfo(int32_t in_serial, const ::aidl::android::hardware::radio::ims::ImsRegistration& in_imsRegistration) = 0;
  virtual ::ndk::ScopedAStatus startImsTraffic(int32_t in_serial, int32_t in_token, ::aidl::android::hardware::radio::ims::ImsTrafficType in_imsTrafficType, ::aidl::android::hardware::radio::AccessNetwork in_accessNetworkType, ::aidl::android::hardware::radio::ims::ImsCall::Direction in_trafficDirection) = 0;
  virtual ::ndk::ScopedAStatus stopImsTraffic(int32_t in_serial, int32_t in_token) = 0;
  virtual ::ndk::ScopedAStatus triggerEpsFallback(int32_t in_serial, ::aidl::android::hardware::radio::ims::EpsFallbackReason in_reason) = 0;
  virtual ::ndk::ScopedAStatus setResponseFunctions(const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsResponse>& in_radioImsResponse, const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsIndication>& in_radioImsIndication) = 0;
  virtual ::ndk::ScopedAStatus sendAnbrQuery(int32_t in_serial, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) = 0;
  virtual ::ndk::ScopedAStatus updateImsCallStatus(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::ImsCall>& in_imsCalls) = 0;
  virtual ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) = 0;
private:
  static std::shared_ptr<IRadioIms> default_impl;
};
class IRadioImsDefault : public IRadioIms {
public:
  ::ndk::ScopedAStatus setSrvccCallInfo(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::SrvccCall>& in_srvccCalls) override;
  ::ndk::ScopedAStatus updateImsRegistrationInfo(int32_t in_serial, const ::aidl::android::hardware::radio::ims::ImsRegistration& in_imsRegistration) override;
  ::ndk::ScopedAStatus startImsTraffic(int32_t in_serial, int32_t in_token, ::aidl::android::hardware::radio::ims::ImsTrafficType in_imsTrafficType, ::aidl::android::hardware::radio::AccessNetwork in_accessNetworkType, ::aidl::android::hardware::radio::ims::ImsCall::Direction in_trafficDirection) override;
  ::ndk::ScopedAStatus stopImsTraffic(int32_t in_serial, int32_t in_token) override;
  ::ndk::ScopedAStatus triggerEpsFallback(int32_t in_serial, ::aidl::android::hardware::radio::ims::EpsFallbackReason in_reason) override;
  ::ndk::ScopedAStatus setResponseFunctions(const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsResponse>& in_radioImsResponse, const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsIndication>& in_radioImsIndication) override;
  ::ndk::ScopedAStatus sendAnbrQuery(int32_t in_serial, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) override;
  ::ndk::ScopedAStatus updateImsCallStatus(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::ImsCall>& in_imsCalls) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  ::ndk::SpAIBinder asBinder() override;
  bool isRemote() override;
};
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
