#pragma once

#include "aidl/android/hardware/gnss/IGnssBatchingCallback.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace hardware {
namespace gnss {
class BpGnssBatchingCallback : public ::ndk::BpCInterface<IGnssBatchingCallback> {
public:
  explicit BpGnssBatchingCallback(const ::ndk::SpAIBinder& binder);
  virtual ~BpGnssBatchingCallback();

  ::ndk::ScopedAStatus gnssLocationBatchCb(const std::vector<::aidl::android::hardware::gnss::GnssLocation>& in_locations) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
};
}  // namespace gnss
}  // namespace hardware
}  // namespace android
}  // namespace aidl
