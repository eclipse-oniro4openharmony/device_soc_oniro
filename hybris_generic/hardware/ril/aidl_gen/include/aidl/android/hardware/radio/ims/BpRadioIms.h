#pragma once

#include "aidl/android/hardware/radio/ims/IRadioIms.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class BpRadioIms : public ::ndk::BpCInterface<IRadioIms> {
public:
  explicit BpRadioIms(const ::ndk::SpAIBinder& binder);
  virtual ~BpRadioIms();

  ::ndk::ScopedAStatus setSrvccCallInfo(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::SrvccCall>& in_srvccCalls) override;
  ::ndk::ScopedAStatus updateImsRegistrationInfo(int32_t in_serial, const ::aidl::android::hardware::radio::ims::ImsRegistration& in_imsRegistration) override;
  ::ndk::ScopedAStatus startImsTraffic(int32_t in_serial, int32_t in_token, ::aidl::android::hardware::radio::ims::ImsTrafficType in_imsTrafficType, ::aidl::android::hardware::radio::AccessNetwork in_accessNetworkType, ::aidl::android::hardware::radio::ims::ImsCall::Direction in_trafficDirection) override;
  ::ndk::ScopedAStatus stopImsTraffic(int32_t in_serial, int32_t in_token) override;
  ::ndk::ScopedAStatus triggerEpsFallback(int32_t in_serial, ::aidl::android::hardware::radio::ims::EpsFallbackReason in_reason) override;
  ::ndk::ScopedAStatus setResponseFunctions(const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsResponse>& in_radioImsResponse, const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsIndication>& in_radioImsIndication) override;
  ::ndk::ScopedAStatus sendAnbrQuery(int32_t in_serial, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) override;
  ::ndk::ScopedAStatus updateImsCallStatus(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::ImsCall>& in_imsCalls) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
};
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
