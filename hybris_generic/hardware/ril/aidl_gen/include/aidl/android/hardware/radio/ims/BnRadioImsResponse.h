#pragma once

#include "aidl/android/hardware/radio/ims/IRadioImsResponse.h"

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
class BnRadioImsResponse : public ::ndk::BnCInterface<IRadioImsResponse> {
public:
  BnRadioImsResponse();
  virtual ~BnRadioImsResponse();
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) final;
protected:
  ::ndk::SpAIBinder createBinder() override;
private:
};
class IRadioImsResponseDelegator : public BnRadioImsResponse {
public:
  explicit IRadioImsResponseDelegator(const std::shared_ptr<IRadioImsResponse> &impl) : _impl(impl) {
     int32_t _impl_ver = 0;
     if (!impl->getInterfaceVersion(&_impl_ver).isOk()) {;
        __assert2(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Delegator failed to get version of the implementation.");
     }
     if (_impl_ver != IRadioImsResponse::version) {
        __assert2(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Mismatched versions of delegator and implementation is not allowed.");
     }
  }

  ::ndk::ScopedAStatus setSrvccCallInfoResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override {
    return _impl->setSrvccCallInfoResponse(in_info);
  }
  ::ndk::ScopedAStatus updateImsRegistrationInfoResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override {
    return _impl->updateImsRegistrationInfoResponse(in_info);
  }
  ::ndk::ScopedAStatus startImsTrafficResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info, const std::optional<::aidl::android::hardware::radio::ims::ConnectionFailureInfo>& in_failureInfo) override {
    return _impl->startImsTrafficResponse(in_info, in_failureInfo);
  }
  ::ndk::ScopedAStatus stopImsTrafficResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override {
    return _impl->stopImsTrafficResponse(in_info);
  }
  ::ndk::ScopedAStatus triggerEpsFallbackResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override {
    return _impl->triggerEpsFallbackResponse(in_info);
  }
  ::ndk::ScopedAStatus sendAnbrQueryResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override {
    return _impl->sendAnbrQueryResponse(in_info);
  }
  ::ndk::ScopedAStatus updateImsCallStatusResponse(const ::aidl::android::hardware::radio::RadioResponseInfo& in_info) override {
    return _impl->updateImsCallStatusResponse(in_info);
  }
protected:
private:
  std::shared_ptr<IRadioImsResponse> _impl;
};

}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
