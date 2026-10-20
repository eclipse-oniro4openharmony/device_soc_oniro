#pragma once

#include "aidl/android/hardware/gnss/IGnssAntennaInfo.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace hardware {
namespace gnss {
class BpGnssAntennaInfo : public ::ndk::BpCInterface<IGnssAntennaInfo> {
public:
  explicit BpGnssAntennaInfo(const ::ndk::SpAIBinder& binder);
  virtual ~BpGnssAntennaInfo();

  ::ndk::ScopedAStatus setCallback(const std::shared_ptr<::aidl::android::hardware::gnss::IGnssAntennaInfoCallback>& in_callback) override;
  ::ndk::ScopedAStatus close() override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
};
}  // namespace gnss
}  // namespace hardware
}  // namespace android
}  // namespace aidl
