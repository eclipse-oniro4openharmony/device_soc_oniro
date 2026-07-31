#include "aidl/android/hardware/radio/ims/IRadioIms.h"

#include <android/binder_parcel_utils.h>
#include <aidl/android/hardware/radio/ims/BnRadioIms.h>
#include <aidl/android/hardware/radio/ims/BnRadioImsIndication.h>
#include <aidl/android/hardware/radio/ims/BnRadioImsResponse.h>
#include <aidl/android/hardware/radio/ims/BpRadioIms.h>
#include <aidl/android/hardware/radio/ims/BpRadioImsIndication.h>
#include <aidl/android/hardware/radio/ims/BpRadioImsResponse.h>
#include <aidl/android/hardware/radio/ims/IRadioImsIndication.h>
#include <aidl/android/hardware/radio/ims/IRadioImsResponse.h>

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
static binder_status_t _aidl_android_hardware_radio_ims_IRadioIms_onTransact(AIBinder* _aidl_binder, transaction_code_t _aidl_code, const AParcel* _aidl_in, AParcel* _aidl_out) {
  (void)_aidl_in;
  (void)_aidl_out;
  binder_status_t _aidl_ret_status = STATUS_UNKNOWN_TRANSACTION;
  std::shared_ptr<BnRadioIms> _aidl_impl = std::static_pointer_cast<BnRadioIms>(::ndk::ICInterface::asInterface(_aidl_binder));
  switch (_aidl_code) {
    case (FIRST_CALL_TRANSACTION + 0 /*setSrvccCallInfo*/): {
      int32_t in_serial;
      std::vector<::aidl::android::hardware::radio::ims::SrvccCall> in_srvccCalls;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_serial);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_srvccCalls);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->setSrvccCallInfo(in_serial, in_srvccCalls);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 1 /*updateImsRegistrationInfo*/): {
      int32_t in_serial;
      ::aidl::android::hardware::radio::ims::ImsRegistration in_imsRegistration;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_serial);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_imsRegistration);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->updateImsRegistrationInfo(in_serial, in_imsRegistration);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 2 /*startImsTraffic*/): {
      int32_t in_serial;
      int32_t in_token;
      ::aidl::android::hardware::radio::ims::ImsTrafficType in_imsTrafficType;
      ::aidl::android::hardware::radio::AccessNetwork in_accessNetworkType;
      ::aidl::android::hardware::radio::ims::ImsCall::Direction in_trafficDirection;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_serial);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_token);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_imsTrafficType);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_accessNetworkType);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_trafficDirection);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->startImsTraffic(in_serial, in_token, in_imsTrafficType, in_accessNetworkType, in_trafficDirection);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 3 /*stopImsTraffic*/): {
      int32_t in_serial;
      int32_t in_token;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_serial);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_token);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->stopImsTraffic(in_serial, in_token);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 4 /*triggerEpsFallback*/): {
      int32_t in_serial;
      ::aidl::android::hardware::radio::ims::EpsFallbackReason in_reason;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_serial);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_reason);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->triggerEpsFallback(in_serial, in_reason);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 5 /*setResponseFunctions*/): {
      std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsResponse> in_radioImsResponse;
      std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsIndication> in_radioImsIndication;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_radioImsResponse);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_radioImsIndication);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->setResponseFunctions(in_radioImsResponse, in_radioImsIndication);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 6 /*sendAnbrQuery*/): {
      int32_t in_serial;
      ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType;
      ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction;
      int32_t in_bitsPerSecond;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_serial);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_mediaType);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_direction);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_bitsPerSecond);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->sendAnbrQuery(in_serial, in_mediaType, in_direction, in_bitsPerSecond);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 7 /*updateImsCallStatus*/): {
      int32_t in_serial;
      std::vector<::aidl::android::hardware::radio::ims::ImsCall> in_imsCalls;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_serial);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_imsCalls);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->updateImsCallStatus(in_serial, in_imsCalls);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 16777214 /*getInterfaceVersion*/): {
      int32_t _aidl_return;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->getInterfaceVersion(&_aidl_return);
      _aidl_ret_status = AParcel_writeStatusHeader(_aidl_out, _aidl_status.get());
      if (_aidl_ret_status != STATUS_OK) break;

      if (!AStatus_isOk(_aidl_status.get())) break;

      _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_out, _aidl_return);
      if (_aidl_ret_status != STATUS_OK) break;

      break;
    }
  }
  return _aidl_ret_status;
}

static AIBinder_Class* _g_aidl_android_hardware_radio_ims_IRadioIms_clazz = ::ndk::ICInterface::defineClass(IRadioIms::descriptor, _aidl_android_hardware_radio_ims_IRadioIms_onTransact);

BpRadioIms::BpRadioIms(const ::ndk::SpAIBinder& binder) : BpCInterface(binder) {}
BpRadioIms::~BpRadioIms() {}

::ndk::ScopedAStatus BpRadioIms::setSrvccCallInfo(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::SrvccCall>& in_srvccCalls) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_serial);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_srvccCalls);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 0 /*setSrvccCallInfo*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioIms::getDefaultImpl()) {
    _aidl_status = IRadioIms::getDefaultImpl()->setSrvccCallInfo(in_serial, in_srvccCalls);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioIms::updateImsRegistrationInfo(int32_t in_serial, const ::aidl::android::hardware::radio::ims::ImsRegistration& in_imsRegistration) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_serial);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_imsRegistration);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 1 /*updateImsRegistrationInfo*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioIms::getDefaultImpl()) {
    _aidl_status = IRadioIms::getDefaultImpl()->updateImsRegistrationInfo(in_serial, in_imsRegistration);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioIms::startImsTraffic(int32_t in_serial, int32_t in_token, ::aidl::android::hardware::radio::ims::ImsTrafficType in_imsTrafficType, ::aidl::android::hardware::radio::AccessNetwork in_accessNetworkType, ::aidl::android::hardware::radio::ims::ImsCall::Direction in_trafficDirection) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_serial);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_token);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_imsTrafficType);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_accessNetworkType);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_trafficDirection);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 2 /*startImsTraffic*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioIms::getDefaultImpl()) {
    _aidl_status = IRadioIms::getDefaultImpl()->startImsTraffic(in_serial, in_token, in_imsTrafficType, in_accessNetworkType, in_trafficDirection);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioIms::stopImsTraffic(int32_t in_serial, int32_t in_token) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_serial);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_token);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 3 /*stopImsTraffic*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioIms::getDefaultImpl()) {
    _aidl_status = IRadioIms::getDefaultImpl()->stopImsTraffic(in_serial, in_token);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioIms::triggerEpsFallback(int32_t in_serial, ::aidl::android::hardware::radio::ims::EpsFallbackReason in_reason) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_serial);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_reason);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 4 /*triggerEpsFallback*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioIms::getDefaultImpl()) {
    _aidl_status = IRadioIms::getDefaultImpl()->triggerEpsFallback(in_serial, in_reason);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioIms::setResponseFunctions(const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsResponse>& in_radioImsResponse, const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsIndication>& in_radioImsIndication) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_radioImsResponse);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_radioImsIndication);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 5 /*setResponseFunctions*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioIms::getDefaultImpl()) {
    _aidl_status = IRadioIms::getDefaultImpl()->setResponseFunctions(in_radioImsResponse, in_radioImsIndication);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioIms::sendAnbrQuery(int32_t in_serial, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_serial);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_mediaType);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_direction);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_bitsPerSecond);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 6 /*sendAnbrQuery*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioIms::getDefaultImpl()) {
    _aidl_status = IRadioIms::getDefaultImpl()->sendAnbrQuery(in_serial, in_mediaType, in_direction, in_bitsPerSecond);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioIms::updateImsCallStatus(int32_t in_serial, const std::vector<::aidl::android::hardware::radio::ims::ImsCall>& in_imsCalls) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_serial);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_imsCalls);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 7 /*updateImsCallStatus*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioIms::getDefaultImpl()) {
    _aidl_status = IRadioIms::getDefaultImpl()->updateImsCallStatus(in_serial, in_imsCalls);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioIms::getInterfaceVersion(int32_t* _aidl_return) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  if (_aidl_cached_version != -1) {
    *_aidl_return = _aidl_cached_version;
    _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
    return _aidl_status;
  }
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 16777214 /*getInterfaceVersion*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    0
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioIms::getDefaultImpl()) {
    _aidl_status = IRadioIms::getDefaultImpl()->getInterfaceVersion(_aidl_return);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AParcel_readStatusHeader(_aidl_out.get(), _aidl_status.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  if (!AStatus_isOk(_aidl_status.get())) goto _aidl_status_return;
  _aidl_ret_status = ::ndk::AParcel_readData(_aidl_out.get(), _aidl_return);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_cached_version = *_aidl_return;
  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
// Source for BnRadioIms
BnRadioIms::BnRadioIms() {}
BnRadioIms::~BnRadioIms() {}
::ndk::SpAIBinder BnRadioIms::createBinder() {
  AIBinder* binder = AIBinder_new(_g_aidl_android_hardware_radio_ims_IRadioIms_clazz, static_cast<void*>(this));
  #ifdef BINDER_STABILITY_SUPPORT
  AIBinder_markVintfStability(binder);
  #endif  // BINDER_STABILITY_SUPPORT
  return ::ndk::SpAIBinder(binder);
}
::ndk::ScopedAStatus BnRadioIms::getInterfaceVersion(int32_t* _aidl_return) {
  *_aidl_return = IRadioIms::version;
  return ::ndk::ScopedAStatus(AStatus_newOk());
}
// Source for IRadioIms
const char* IRadioIms::descriptor = "android.hardware.radio.ims.IRadioIms";
IRadioIms::IRadioIms() {}
IRadioIms::~IRadioIms() {}


std::shared_ptr<IRadioIms> IRadioIms::fromBinder(const ::ndk::SpAIBinder& binder) {
  if (!AIBinder_associateClass(binder.get(), _g_aidl_android_hardware_radio_ims_IRadioIms_clazz)) { return nullptr; }
  std::shared_ptr<::ndk::ICInterface> interface = ::ndk::ICInterface::asInterface(binder.get());
  if (interface) {
    return std::static_pointer_cast<IRadioIms>(interface);
  }
  return ::ndk::SharedRefBase::make<BpRadioIms>(binder);
}

binder_status_t IRadioIms::writeToParcel(AParcel* parcel, const std::shared_ptr<IRadioIms>& instance) {
  return AParcel_writeStrongBinder(parcel, instance ? instance->asBinder().get() : nullptr);
}
binder_status_t IRadioIms::readFromParcel(const AParcel* parcel, std::shared_ptr<IRadioIms>* instance) {
  ::ndk::SpAIBinder binder;
  binder_status_t status = AParcel_readStrongBinder(parcel, binder.getR());
  if (status != STATUS_OK) return status;
  *instance = IRadioIms::fromBinder(binder);
  return STATUS_OK;
}
bool IRadioIms::setDefaultImpl(const std::shared_ptr<IRadioIms>& impl) {
  // Only one user of this interface can use this function
  // at a time. This is a heuristic to detect if two different
  // users in the same process use this function.
  assert(!IRadioIms::default_impl);
  if (impl) {
    IRadioIms::default_impl = impl;
    return true;
  }
  return false;
}
const std::shared_ptr<IRadioIms>& IRadioIms::getDefaultImpl() {
  return IRadioIms::default_impl;
}
std::shared_ptr<IRadioIms> IRadioIms::default_impl = nullptr;
::ndk::ScopedAStatus IRadioImsDefault::setSrvccCallInfo(int32_t /*in_serial*/, const std::vector<::aidl::android::hardware::radio::ims::SrvccCall>& /*in_srvccCalls*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsDefault::updateImsRegistrationInfo(int32_t /*in_serial*/, const ::aidl::android::hardware::radio::ims::ImsRegistration& /*in_imsRegistration*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsDefault::startImsTraffic(int32_t /*in_serial*/, int32_t /*in_token*/, ::aidl::android::hardware::radio::ims::ImsTrafficType /*in_imsTrafficType*/, ::aidl::android::hardware::radio::AccessNetwork /*in_accessNetworkType*/, ::aidl::android::hardware::radio::ims::ImsCall::Direction /*in_trafficDirection*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsDefault::stopImsTraffic(int32_t /*in_serial*/, int32_t /*in_token*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsDefault::triggerEpsFallback(int32_t /*in_serial*/, ::aidl::android::hardware::radio::ims::EpsFallbackReason /*in_reason*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsDefault::setResponseFunctions(const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsResponse>& /*in_radioImsResponse*/, const std::shared_ptr<::aidl::android::hardware::radio::ims::IRadioImsIndication>& /*in_radioImsIndication*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsDefault::sendAnbrQuery(int32_t /*in_serial*/, ::aidl::android::hardware::radio::ims::ImsStreamType /*in_mediaType*/, ::aidl::android::hardware::radio::ims::ImsStreamDirection /*in_direction*/, int32_t /*in_bitsPerSecond*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsDefault::updateImsCallStatus(int32_t /*in_serial*/, const std::vector<::aidl::android::hardware::radio::ims::ImsCall>& /*in_imsCalls*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsDefault::getInterfaceVersion(int32_t* _aidl_return) {
  *_aidl_return = 0;
  return ::ndk::ScopedAStatus(AStatus_newOk());
}
::ndk::SpAIBinder IRadioImsDefault::asBinder() {
  return ::ndk::SpAIBinder();
}
bool IRadioImsDefault::isRemote() {
  return false;
}
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
