#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_interface_utils.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl {
namespace android {
namespace hardware {
namespace gnss {
class IAGnssRilCallbackDelegator;

class IAGnssRilCallback : public ::ndk::ICInterface {
public:
  typedef IAGnssRilCallbackDelegator DefaultDelegator;
  static const char* descriptor;
  IAGnssRilCallback();
  virtual ~IAGnssRilCallback();

  static const int32_t version = 3;
  static constexpr uint32_t TRANSACTION_requestSetIdCb = FIRST_CALL_TRANSACTION + 0;
  static constexpr uint32_t TRANSACTION_requestRefLocCb = FIRST_CALL_TRANSACTION + 1;

  static std::shared_ptr<IAGnssRilCallback> fromBinder(const ::ndk::SpAIBinder& binder);
  static binder_status_t writeToParcel(AParcel* parcel, const std::shared_ptr<IAGnssRilCallback>& instance);
  static binder_status_t readFromParcel(const AParcel* parcel, std::shared_ptr<IAGnssRilCallback>* instance);
  static bool setDefaultImpl(const std::shared_ptr<IAGnssRilCallback>& impl);
  static const std::shared_ptr<IAGnssRilCallback>& getDefaultImpl();
  virtual ::ndk::ScopedAStatus requestSetIdCb(int32_t in_setIdflag) = 0;
  virtual ::ndk::ScopedAStatus requestRefLocCb() = 0;
  virtual ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) = 0;
private:
  static std::shared_ptr<IAGnssRilCallback> default_impl;
};
class IAGnssRilCallbackDefault : public IAGnssRilCallback {
public:
  ::ndk::ScopedAStatus requestSetIdCb(int32_t in_setIdflag) override;
  ::ndk::ScopedAStatus requestRefLocCb() override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  ::ndk::SpAIBinder asBinder() override;
  bool isRemote() override;
};
}  // namespace gnss
}  // namespace hardware
}  // namespace android
}  // namespace aidl
