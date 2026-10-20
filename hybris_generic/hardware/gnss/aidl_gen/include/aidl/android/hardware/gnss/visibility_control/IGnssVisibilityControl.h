#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_interface_utils.h>
#include <aidl/android/hardware/gnss/visibility_control/IGnssVisibilityControlCallback.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::gnss::visibility_control {
class IGnssVisibilityControlCallback;
}  // namespace aidl::android::hardware::gnss::visibility_control
namespace aidl {
namespace android {
namespace hardware {
namespace gnss {
namespace visibility_control {
class IGnssVisibilityControlDelegator;

class IGnssVisibilityControl : public ::ndk::ICInterface {
public:
  typedef IGnssVisibilityControlDelegator DefaultDelegator;
  static const char* descriptor;
  IGnssVisibilityControl();
  virtual ~IGnssVisibilityControl();

  static const int32_t version = 3;
  static constexpr uint32_t TRANSACTION_enableNfwLocationAccess = FIRST_CALL_TRANSACTION + 0;
  static constexpr uint32_t TRANSACTION_setCallback = FIRST_CALL_TRANSACTION + 1;

  static std::shared_ptr<IGnssVisibilityControl> fromBinder(const ::ndk::SpAIBinder& binder);
  static binder_status_t writeToParcel(AParcel* parcel, const std::shared_ptr<IGnssVisibilityControl>& instance);
  static binder_status_t readFromParcel(const AParcel* parcel, std::shared_ptr<IGnssVisibilityControl>* instance);
  static bool setDefaultImpl(const std::shared_ptr<IGnssVisibilityControl>& impl);
  static const std::shared_ptr<IGnssVisibilityControl>& getDefaultImpl();
  virtual ::ndk::ScopedAStatus enableNfwLocationAccess(const std::vector<std::string>& in_proxyApps) = 0;
  virtual ::ndk::ScopedAStatus setCallback(const std::shared_ptr<::aidl::android::hardware::gnss::visibility_control::IGnssVisibilityControlCallback>& in_callback) = 0;
  virtual ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) = 0;
private:
  static std::shared_ptr<IGnssVisibilityControl> default_impl;
};
class IGnssVisibilityControlDefault : public IGnssVisibilityControl {
public:
  ::ndk::ScopedAStatus enableNfwLocationAccess(const std::vector<std::string>& in_proxyApps) override;
  ::ndk::ScopedAStatus setCallback(const std::shared_ptr<::aidl::android::hardware::gnss::visibility_control::IGnssVisibilityControlCallback>& in_callback) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  ::ndk::SpAIBinder asBinder() override;
  bool isRemote() override;
};
}  // namespace visibility_control
}  // namespace gnss
}  // namespace hardware
}  // namespace android
}  // namespace aidl
