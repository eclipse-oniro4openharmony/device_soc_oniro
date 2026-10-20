#pragma once

#include "aidl/android/hardware/gnss/IAGnssCallback.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace hardware {
namespace gnss {
class BpAGnssCallback : public ::ndk::BpCInterface<IAGnssCallback> {
public:
  explicit BpAGnssCallback(const ::ndk::SpAIBinder& binder);
  virtual ~BpAGnssCallback();

  ::ndk::ScopedAStatus agnssStatusCb(::aidl::android::hardware::gnss::IAGnssCallback::AGnssType in_type, ::aidl::android::hardware::gnss::IAGnssCallback::AGnssStatusValue in_status) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
};
}  // namespace gnss
}  // namespace hardware
}  // namespace android
}  // namespace aidl
