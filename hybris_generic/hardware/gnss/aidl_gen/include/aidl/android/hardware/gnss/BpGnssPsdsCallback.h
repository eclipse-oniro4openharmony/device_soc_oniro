#pragma once

#include "aidl/android/hardware/gnss/IGnssPsdsCallback.h"

#include <android/binder_ibinder.h>

namespace aidl {
namespace android {
namespace hardware {
namespace gnss {
class BpGnssPsdsCallback : public ::ndk::BpCInterface<IGnssPsdsCallback> {
public:
  explicit BpGnssPsdsCallback(const ::ndk::SpAIBinder& binder);
  virtual ~BpGnssPsdsCallback();

  ::ndk::ScopedAStatus downloadRequestCb(::aidl::android::hardware::gnss::PsdsType in_psdsType) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  int32_t _aidl_cached_version = -1;
};
}  // namespace gnss
}  // namespace hardware
}  // namespace android
}  // namespace aidl
