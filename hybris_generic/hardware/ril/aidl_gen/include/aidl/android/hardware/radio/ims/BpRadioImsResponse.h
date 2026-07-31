#pragma once

#include "aidl/android/hardware/radio/ims/IRadioImsResponse.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class BpRadioImsResponse : public ::ndk::BpCInterface<IRadioImsResponse> {
public:
  explicit BpRadioImsResponse(const ::ndk::SpAIBinder& binder);
  virtual ~BpRadioImsResponse();

  ::ndk::ScopedAStatus setSrvccCallInfoResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus updateImsRegistrationInfoResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus startImsTrafficResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info, const std::optional<::aidl::android::hardware::radio::ims::ConnectionFailureInfo>& in_failureInfo) override;
  ::ndk::ScopedAStatus stopImsTrafficResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus triggerEpsFallbackResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus sendAnbrQueryResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus updateImsCallStatusResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
};
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
