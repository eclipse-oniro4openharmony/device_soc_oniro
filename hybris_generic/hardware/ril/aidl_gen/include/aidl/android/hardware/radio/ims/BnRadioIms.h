#pragma once

#include "aidl/android/hardware/radio/ims/IRadioIms.h"

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
class BnRadioIms : public ::ndk::BnCInterface<IRadioIms> {
public:
  BnRadioIms();
  virtual ~BnRadioIms();
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) final;
protected:
  ::ndk::SpAIBinder createBinder() override;
private:
};
class IRadioImsDelegator : public BnRadioIms {
public:
  explicit IRadioImsDelegator(const std::shared_ptr<IRadioIms> &impl) : _impl(impl) {
     int32_t _impl_ver = 0;
     if (!impl->getInterfaceVersion(&_impl_ver).isOk()) {;
        __assert2(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Delegator failed to get version of the implementation.");
     }
     if (_impl_ver != IRadioIms::version) {
        __assert2(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Mismatched versions of delegator and implementation is not allowed.");
     }
  }

  ::ndk::ScopedAStatus setSrvccCallInfo(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::SrvccCall>& in_srvccCalls) override {
    return _impl->setSrvccCallInfo(in_serial, in_srvccCalls);
  }
  ::ndk::ScopedAStatus updateImsRegistrationInfo(int32_t in_serial, const ::aidl::android::hardware::radio::ims::ImsRegistration& in_imsRegistration) override {
    return _impl->updateImsRegistrationInfo(in_serial, in_imsRegistration);
  }
  ::ndk::ScopedAStatus startImsTraffic(int32_t in_serial, int32_t in_token, ::aidl::android::hardware::radio::ims::ImsTrafficType in_imsTrafficType, ::aidl::android::hardware::radio::AccessNetwork in_accessNetworkType, ::aidl::android::hardware::radio::ims::ImsCall::Direction in_trafficDirection) override {
    return _impl->startImsTraffic(in_serial, in_token, in_imsTrafficType, in_accessNetworkType, in_trafficDirection);
  }
  ::ndk::ScopedAStatus stopImsTraffic(int32_t in_serial, int32_t in_token) override {
    return _impl->stopImsTraffic(in_serial, in_token);
  }
  ::ndk::ScopedAStatus triggerEpsFallback(int32_t in_serial, ::aidl::android::hardware::radio::ims::EpsFallbackReason in_reason) override {
    return _impl->triggerEpsFallback(in_serial, in_reason);
  }
  ::ndk::ScopedAStatus setResponseFunctions(const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsResponse>& in_radioImsResponse, const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsIndication>& in_radioImsIndication) override {
    return _impl->setResponseFunctions(in_radioImsResponse, in_radioImsIndication);
  }
  ::ndk::ScopedAStatus sendAnbrQuery(int32_t in_serial, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) override {
    return _impl->sendAnbrQuery(in_serial, in_mediaType, in_direction, in_bitsPerSecond);
  }
  ::ndk::ScopedAStatus updateImsCallStatus(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::ImsCall>& in_imsCalls) override {
    return _impl->updateImsCallStatus(in_serial, in_imsCalls);
  }
protected:
private:
  std::shared_ptr<IRadioIms> _impl;
};

}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
