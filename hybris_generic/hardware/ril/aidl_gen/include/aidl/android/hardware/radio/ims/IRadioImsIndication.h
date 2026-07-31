#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_interface_utils.h>
#include <aidl/android/hardware/radio/RadioIndicationType.h>
#include <aidl/android/hardware/radio/ims/ConnectionFailureInfo.h>
#include <aidl/android/hardware/radio/ims/ImsDeregistrationReason.h>
#include <aidl/android/hardware/radio/ims/ImsStreamDirection.h>
#include <aidl/android/hardware/radio/ims/ImsStreamType.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::radio::ims {
class ConnectionFailureInfo;
}  // namespace aidl::android::hardware::radio::ims
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class IRadioImsIndicationDelegator;

class IRadioImsIndication : public ::ndk::ICInterface {
public:
  typedef IRadioImsIndicationDelegator DefaultDelegator;
  static const char* descriptor;
  IRadioImsIndication();
  virtual ~IRadioImsIndication();

  static const int32_t version = 1;
  static constexpr uint32_t TRANSACTION_onConnectionSetupFailure = FIRST_CALL_TRANSACTION + 0;
  static constexpr uint32_t TRANSACTION_notifyAnbr = FIRST_CALL_TRANSACTION + 1;
  static constexpr uint32_t TRANSACTION_triggerImsDeregistration = FIRST_CALL_TRANSACTION + 2;

  static std::shared_ptr<IRadioImsIndication> fromBinder(const ::ndk::SpAIBinder& binder);
  static binder_status_t writeToParcel(AParcel* parcel, const std::shared_ptr<IRadioImsIndication>& instance);
  static binder_status_t readFromParcel(const AParcel* parcel, std::shared_ptr<IRadioImsIndication>* instance);
  static bool setDefaultImpl(const std::shared_ptr<IRadioImsIndication>& impl);
  static const std::shared_ptr<IRadioImsIndication>& getDefaultImpl();
  virtual ::ndk::ScopedAStatus onConnectionSetupFailure(::aidl::android::hardware::radio::RadioIndicationType in_type, int32_t in_token, const ::aidl::android::hardware::radio::ims::ConnectionFailureInfo& in_info) = 0;
  virtual ::ndk::ScopedAStatus notifyAnbr(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) = 0;
  virtual ::ndk::ScopedAStatus triggerImsDeregistration(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsDeregistrationReason in_reason) = 0;
  virtual ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) = 0;
private:
  static std::shared_ptr<IRadioImsIndication> default_impl;
};
class IRadioImsIndicationDefault : public IRadioImsIndication {
public:
  ::ndk::ScopedAStatus onConnectionSetupFailure(::aidl::android::hardware::radio::RadioIndicationType in_type, int32_t in_token, const ::aidl::android::hardware::radio::ims::ConnectionFailureInfo& in_info) override;
  ::ndk::ScopedAStatus notifyAnbr(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) override;
  ::ndk::ScopedAStatus triggerImsDeregistration(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsDeregistrationReason in_reason) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  ::ndk::SpAIBinder asBinder() override;
  bool isRemote() override;
};
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
