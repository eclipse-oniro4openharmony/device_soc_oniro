#pragma once

#include "aidl/android/hardware/radio/ims/IRadioImsIndication.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class BpRadioImsIndication : public ::ndk::BpCInterface<IRadioImsIndication> {
public:
  explicit BpRadioImsIndication(const ::ndk::SpAIBinder& binder);
  virtual ~BpRadioImsIndication();

  ::ndk::ScopedAStatus onConnectionSetupFailure(::aidl::android::hardware::radio::RadioIndicationType in_type, int32_t in_token, const ::aidl::android::hardware::radio::ims::ConnectionFailureInfo& in_info) override;
  ::ndk::ScopedAStatus notifyAnbr(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) override;
  ::ndk::ScopedAStatus triggerImsDeregistration(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsDeregistrationReason in_reason) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
};
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
