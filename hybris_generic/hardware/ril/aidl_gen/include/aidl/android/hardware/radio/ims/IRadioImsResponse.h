#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_interface_utils.h>
#include <aidl/android/hardware/radio/RadioResponseInfo.h>
#include <aidl/android/hardware/radio/ims/ConnectionFailureInfo.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::radio {
class RadioResponseInfo;
}  // namespace aidl::android::hardware::radio
namespace aidl::android::hardware::radio::ims {
class ConnectionFailureInfo;
}  // namespace aidl::android::hardware::radio::ims
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class IRadioImsResponseDelegator;

class IRadioImsResponse : public ::ndk::ICInterface {
public:
  typedef IRadioImsResponseDelegator DefaultDelegator;
  static const char* descriptor;
  IRadioImsResponse();
  virtual ~IRadioImsResponse();

  static const int32_t version = 1;
  static constexpr uint32_t TRANSACTION_setSrvccCallInfoResponse = FIRST_CALL_TRANSACTION + 0;
  static constexpr uint32_t TRANSACTION_updateImsRegistrationInfoResponse = FIRST_CALL_TRANSACTION + 1;
  static constexpr uint32_t TRANSACTION_startImsTrafficResponse = FIRST_CALL_TRANSACTION + 2;
  static constexpr uint32_t TRANSACTION_stopImsTrafficResponse = FIRST_CALL_TRANSACTION + 3;
  static constexpr uint32_t TRANSACTION_triggerEpsFallbackResponse = FIRST_CALL_TRANSACTION + 4;
  static constexpr uint32_t TRANSACTION_sendAnbrQueryResponse = FIRST_CALL_TRANSACTION + 5;
  static constexpr uint32_t TRANSACTION_updateImsCallStatusResponse = FIRST_CALL_TRANSACTION + 6;

  static std::shared_ptr<IRadioImsResponse> fromBinder(const ::ndk::SpAIBinder& binder);
  static binder_status_t writeToParcel(AParcel* parcel, const std::shared_ptr<IRadioImsResponse>& instance);
  static binder_status_t readFromParcel(const AParcel* parcel, std::shared_ptr<IRadioImsResponse>* instance);
  static bool setDefaultImpl(const std::shared_ptr<IRadioImsResponse>& impl);
  static const std::shared_ptr<IRadioImsResponse>& getDefaultImpl();
  virtual ::ndk::ScopedAStatus setSrvccCallInfoResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) = 0;
  virtual ::ndk::ScopedAStatus updateImsRegistrationInfoResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) = 0;
  virtual ::ndk::ScopedAStatus startImsTrafficResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info, const std::optional<::aidl::android::hardware::radio::ims::ConnectionFailureInfo>& in_failureInfo) = 0;
  virtual ::ndk::ScopedAStatus stopImsTrafficResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) = 0;
  virtual ::ndk::ScopedAStatus triggerEpsFallbackResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) = 0;
  virtual ::ndk::ScopedAStatus sendAnbrQueryResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) = 0;
  virtual ::ndk::ScopedAStatus updateImsCallStatusResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) = 0;
  virtual ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) = 0;
private:
  static std::shared_ptr<IRadioImsResponse> default_impl;
};
class IRadioImsResponseDefault : public IRadioImsResponse {
public:
  ::ndk::ScopedAStatus setSrvccCallInfoResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus updateImsRegistrationInfoResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus startImsTrafficResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info, const std::optional<::aidl::android::hardware::radio::ims::ConnectionFailureInfo>& in_failureInfo) override;
  ::ndk::ScopedAStatus stopImsTrafficResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus triggerEpsFallbackResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus sendAnbrQueryResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus updateImsCallStatusResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  ::ndk::SpAIBinder asBinder() override;
  bool isRemote() override;
};
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
