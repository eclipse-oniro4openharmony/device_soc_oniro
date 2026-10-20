#pragma once

#include "aidl/android/hardware/gnss/IGnssDebug.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace hardware {
namespace gnss {
class BpGnssDebug : public ::ndk::BpCInterface<IGnssDebug> {
public:
  explicit BpGnssDebug(const ::ndk::SpAIBinder& binder);
  virtual ~BpGnssDebug();

  ::ndk::ScopedAStatus getDebugData(::aidl::android::hardware::gnss::IGnssDebug::DebugData* _aidl_return) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
};
}  // namespace gnss
}  // namespace hardware
}  // namespace android
}  // namespace aidl
