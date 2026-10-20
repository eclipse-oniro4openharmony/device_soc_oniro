#pragma once

#include "aidl/android/hardware/gnss/IGnssAntennaInfoCallback.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace hardware {
namespace gnss {
class BpGnssAntennaInfoCallback : public ::ndk::BpCInterface<IGnssAntennaInfoCallback> {
public:
  explicit BpGnssAntennaInfoCallback(const ::ndk::SpAIBinder& binder);
  virtual ~BpGnssAntennaInfoCallback();

  ::ndk::ScopedAStatus gnssAntennaInfoCb(const std::vector<::aidl::android::hardware::gnss::IGnssAntennaInfoCallback::GnssAntennaInfo>& in_gnssAntennaInfos) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
};
}  // namespace gnss
}  // namespace hardware
}  // namespace android
}  // namespace aidl
