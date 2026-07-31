#pragma once

#include "aidl/android/hardware/radio/ims/IRadioImsIndication.h"

#include <android/binder_ibinder.h>
#include <cassert>

#ifndef __BIONIC__
#ifndef __assert2
#define __assert2(a,b,c,d) ((void)0)
#endif
#endif

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
class BnRadioImsIndication : public ::ndk::BnCInterface<IRadioImsIndication> {
public:
  BnRadioImsIndication();
  virtual ~BnRadioImsIndication();
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) final;
protected:
  ::ndk::SpAIBinder createBinder() override;
private:
};
class IRadioImsIndicationDelegator : public BnRadioImsIndication {
public:
  explicit IRadioImsIndicationDelegator(const std::shared_ptr<IRadioImsIndication> &impl) : _impl(impl) {
     int32_t _impl_ver = 0;
     if (!impl->getInterfaceVersion(&_impl_ver).isOk()) {;
        __assert2(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Delegator failed to get version of the implementation.");
     }
     if (_impl_ver != IRadioImsIndication::version) {
        __assert2(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Mismatched versions of delegator and implementation is not allowed.");
     }
  }

  ::ndk::ScopedAStatus onConnectionSetupFailure(::aidl::android::hardware::radio::RadioIndicationType in_type, int32_t in_token, const ::aidl::android::hardware::radio::ims::ConnectionFailureInfo& in_info) override {
    return _impl->onConnectionSetupFailure(in_type, in_token, in_info);
  }
  ::ndk::ScopedAStatus notifyAnbr(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) override {
    return _impl->notifyAnbr(in_type, in_mediaType, in_direction, in_bitsPerSecond);
  }
  ::ndk::ScopedAStatus triggerImsDeregistration(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsDeregistrationReason in_reason) override {
    return _impl->triggerImsDeregistration(in_type, in_reason);
  }
protected:
private:
  std::shared_ptr<IRadioImsIndication> _impl;
};

}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
