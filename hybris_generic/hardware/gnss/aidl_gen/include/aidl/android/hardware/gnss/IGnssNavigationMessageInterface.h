#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_interface_utils.h>
#include <aidl/android/hardware/gnss/IGnssNavigationMessageCallback.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::gnss {
class IGnssNavigationMessageCallback;
}  // namespace aidl::android::hardware::gnss
namespace aidl {
namespace android {
namespace hardware {
namespace gnss {
class IGnssNavigationMessageInterfaceDelegator;

class IGnssNavigationMessageInterface : public ::ndk::ICInterface {
public:
  typedef IGnssNavigationMessageInterfaceDelegator DefaultDelegator;
  static const char* descriptor;
  IGnssNavigationMessageInterface();
  virtual ~IGnssNavigationMessageInterface();

  static const int32_t version = 3;
  static constexpr uint32_t TRANSACTION_setCallback = FIRST_CALL_TRANSACTION + 0;
  static constexpr uint32_t TRANSACTION_close = FIRST_CALL_TRANSACTION + 1;

  static std::shared_ptr<IGnssNavigationMessageInterface> fromBinder(const ::ndk::SpAIBinder& binder);
  static binder_status_t writeToParcel(AParcel* parcel, const std::shared_ptr<IGnssNavigationMessageInterface>& instance);
  static binder_status_t readFromParcel(const AParcel* parcel, std::shared_ptr<IGnssNavigationMessageInterface>* instance);
  static bool setDefaultImpl(const std::shared_ptr<IGnssNavigationMessageInterface>& impl);
  static const std::shared_ptr<IGnssNavigationMessageInterface>& getDefaultImpl();
  virtual ::ndk::ScopedAStatus setCallback(const std::shared_ptr<::aidl::android::hardware::gnss::IGnssNavigationMessageCallback>& in_callback) = 0;
  virtual ::ndk::ScopedAStatus close() = 0;
  virtual ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) = 0;
private:
  static std::shared_ptr<IGnssNavigationMessageInterface> default_impl;
};
class IGnssNavigationMessageInterfaceDefault : public IGnssNavigationMessageInterface {
public:
  ::ndk::ScopedAStatus setCallback(const std::shared_ptr<::aidl::android::hardware::gnss::IGnssNavigationMessageCallback>& in_callback) override;
  ::ndk::ScopedAStatus close() override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  ::ndk::SpAIBinder asBinder() override;
  bool isRemote() override;
};
}  // namespace gnss
}  // namespace hardware
}  // namespace android
}  // namespace aidl
