#include "aidl/android/hardware/radio/ims/IRadioImsIndication.h"

#include <android/binder_parcel_utils.h>
#include <aidl/android/hardware/radio/ims/BnRadioImsIndication.h>
#include <aidl/android/hardware/radio/ims/BpRadioImsIndication.h>

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace ims {
static binder_status_t _aidl_android_hardware_radio_ims_IRadioImsIndication_onTransact(AIBinder* _aidl_binder, transaction_code_t _aidl_code, const AParcel* _aidl_in, AParcel* _aidl_out) {
  (void)_aidl_in;
  (void)_aidl_out;
  binder_status_t _aidl_ret_status = STATUS_UNKNOWN_TRANSACTION;
  std::shared_ptr<BnRadioImsIndication> _aidl_impl = std::static_pointer_cast<BnRadioImsIndication>(::ndk::ICInterface::asInterface(_aidl_binder));
  switch (_aidl_code) {
    case (FIRST_CALL_TRANSACTION + 0 /*onConnectionSetupFailure*/): {
      ::aidl::android::hardware::radio::RadioIndicationType in_type;
      int32_t in_token;
      ::aidl::android::hardware::radio::ims::ConnectionFailureInfo in_info;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_type);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_token);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_info);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->onConnectionSetupFailure(in_type, in_token, in_info);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 1 /*notifyAnbr*/): {
      ::aidl::android::hardware::radio::RadioIndicationType in_type;
      ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType;
      ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction;
      int32_t in_bitsPerSecond;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_type);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_mediaType);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_direction);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_bitsPerSecond);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->notifyAnbr(in_type, in_mediaType, in_direction, in_bitsPerSecond);
      _aidl_ret_status = STATUS_OK;
      break;
    }
    case (FIRST_CALL_TRANSACTION + 2 /*triggerImsDeregistration*/): {
      ::aidl::android::hardware::radio::RadioIndicationType in_type;
      ::aidl::android::hardware::radio::ims::ImsDeregistrationReason in_reason;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_type);
      if (_aidl_ret_status != STATUS_OK) break;

      _aidl_ret_status = ::ndk::AParcel_readData(_aidl_in, &in_reason);
      if (_aidl_ret_status != STATUS_OK) break;

      ::ndk::ScopedAStatus _aidl_status = _aidl_impl->triggerImsDeregistration(in_type, in_reason);
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

static AIBinder_Class* _g_aidl_android_hardware_radio_ims_IRadioImsIndication_clazz = ::ndk::ICInterface::defineClass(IRadioImsIndication::descriptor, _aidl_android_hardware_radio_ims_IRadioImsIndication_onTransact);

BpRadioImsIndication::BpRadioImsIndication(const ::ndk::SpAIBinder& binder) : BpCInterface(binder) {}
BpRadioImsIndication::~BpRadioImsIndication() {}

::ndk::ScopedAStatus BpRadioImsIndication::onConnectionSetupFailure(::aidl::android::hardware::radio::RadioIndicationType in_type, int32_t in_token, const ::aidl::android::hardware::radio::ims::ConnectionFailureInfo& in_info) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_type);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_token);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_info);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 0 /*onConnectionSetupFailure*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioImsIndication::getDefaultImpl()) {
    _aidl_status = IRadioImsIndication::getDefaultImpl()->onConnectionSetupFailure(in_type, in_token, in_info);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioImsIndication::notifyAnbr(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsStreamType in_mediaType, ::aidl::android::hardware::radio::ims::ImsStreamDirection in_direction, int32_t in_bitsPerSecond) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_type);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_mediaType);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_direction);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_bitsPerSecond);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 1 /*notifyAnbr*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioImsIndication::getDefaultImpl()) {
    _aidl_status = IRadioImsIndication::getDefaultImpl()->notifyAnbr(in_type, in_mediaType, in_direction, in_bitsPerSecond);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioImsIndication::triggerImsDeregistration(::aidl::android::hardware::radio::RadioIndicationType in_type, ::aidl::android::hardware::radio::ims::ImsDeregistrationReason in_reason) {
  binder_status_t _aidl_ret_status = STATUS_OK;
  ::ndk::ScopedAStatus _aidl_status;
  ::ndk::ScopedAParcel _aidl_in;
  ::ndk::ScopedAParcel _aidl_out;

  _aidl_ret_status = AIBinder_prepareTransaction(asBinder().get(), _aidl_in.getR());
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_type);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = ::ndk::AParcel_writeData(_aidl_in.get(), in_reason);
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_ret_status = AIBinder_transact(
    asBinder().get(),
    (FIRST_CALL_TRANSACTION + 2 /*triggerImsDeregistration*/),
    _aidl_in.getR(),
    _aidl_out.getR(),
    FLAG_ONEWAY
    #ifdef BINDER_STABILITY_SUPPORT
    | FLAG_PRIVATE_LOCAL
    #endif  // BINDER_STABILITY_SUPPORT
    );
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioImsIndication::getDefaultImpl()) {
    _aidl_status = IRadioImsIndication::getDefaultImpl()->triggerImsDeregistration(in_type, in_reason);
    goto _aidl_status_return;
  }
  if (_aidl_ret_status != STATUS_OK) goto _aidl_error;

  _aidl_error:
  _aidl_status.set(AStatus_fromStatus(_aidl_ret_status));
  _aidl_status_return:
  return _aidl_status;
}
::ndk::ScopedAStatus BpRadioImsIndication::getInterfaceVersion(int32_t* _aidl_return) {
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
  if (_aidl_ret_status == STATUS_UNKNOWN_TRANSACTION && IRadioImsIndication::getDefaultImpl()) {
    _aidl_status = IRadioImsIndication::getDefaultImpl()->getInterfaceVersion(_aidl_return);
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
// Source for BnRadioImsIndication
BnRadioImsIndication::BnRadioImsIndication() {}
BnRadioImsIndication::~BnRadioImsIndication() {}
::ndk::SpAIBinder BnRadioImsIndication::createBinder() {
  AIBinder* binder = AIBinder_new(_g_aidl_android_hardware_radio_ims_IRadioImsIndication_clazz, static_cast<void*>(this));
  #ifdef BINDER_STABILITY_SUPPORT
  AIBinder_markVintfStability(binder);
  #endif  // BINDER_STABILITY_SUPPORT
  return ::ndk::SpAIBinder(binder);
}
::ndk::ScopedAStatus BnRadioImsIndication::getInterfaceVersion(int32_t* _aidl_return) {
  *_aidl_return = IRadioImsIndication::version;
  return ::ndk::ScopedAStatus(AStatus_newOk());
}
// Source for IRadioImsIndication
const char* IRadioImsIndication::descriptor = "android.hardware.radio.ims.IRadioImsIndication";
IRadioImsIndication::IRadioImsIndication() {}
IRadioImsIndication::~IRadioImsIndication() {}


std::shared_ptr<IRadioImsIndication> IRadioImsIndication::fromBinder(const ::ndk::SpAIBinder& binder) {
  if (!AIBinder_associateClass(binder.get(), _g_aidl_android_hardware_radio_ims_IRadioImsIndication_clazz)) { return nullptr; }
  std::shared_ptr<::ndk::ICInterface> interface = ::ndk::ICInterface::asInterface(binder.get());
  if (interface) {
    return std::static_pointer_cast<IRadioImsIndication>(interface);
  }
  return ::ndk::SharedRefBase::make<BpRadioImsIndication>(binder);
}

binder_status_t IRadioImsIndication::writeToParcel(AParcel* parcel, const std::shared_ptr<IRadioImsIndication>& instance) {
  return AParcel_writeStrongBinder(parcel, instance ? instance->asBinder().get() : nullptr);
}
binder_status_t IRadioImsIndication::readFromParcel(const AParcel* parcel, std::shared_ptr<IRadioImsIndication>* instance) {
  ::ndk::SpAIBinder binder;
  binder_status_t status = AParcel_readStrongBinder(parcel, binder.getR());
  if (status != STATUS_OK) return status;
  *instance = IRadioImsIndication::fromBinder(binder);
  return STATUS_OK;
}
bool IRadioImsIndication::setDefaultImpl(const std::shared_ptr<IRadioImsIndication>& impl) {
  // Only one user of this interface can use this function
  // at a time. This is a heuristic to detect if two different
  // users in the same process use this function.
  assert(!IRadioImsIndication::default_impl);
  if (impl) {
    IRadioImsIndication::default_impl = impl;
    return true;
  }
  return false;
}
const std::shared_ptr<IRadioImsIndication>& IRadioImsIndication::getDefaultImpl() {
  return IRadioImsIndication::default_impl;
}
std::shared_ptr<IRadioImsIndication> IRadioImsIndication::default_impl = nullptr;
::ndk::ScopedAStatus IRadioImsIndicationDefault::onConnectionSetupFailure(::aidl::android::hardware::radio::RadioIndicationType /*in_type*/, int32_t /*in_token*/, const ::aidl::android::hardware::radio::ims::ConnectionFailureInfo& /*in_info*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsIndicationDefault::notifyAnbr(::aidl::android::hardware::radio::RadioIndicationType /*in_type*/, ::aidl::android::hardware::radio::ims::ImsStreamType /*in_mediaType*/, ::aidl::android::hardware::radio::ims::ImsStreamDirection /*in_direction*/, int32_t /*in_bitsPerSecond*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsIndicationDefault::triggerImsDeregistration(::aidl::android::hardware::radio::RadioIndicationType /*in_type*/, ::aidl::android::hardware::radio::ims::ImsDeregistrationReason /*in_reason*/) {
  ::ndk::ScopedAStatus _aidl_status;
  _aidl_status.set(AStatus_fromStatus(STATUS_UNKNOWN_TRANSACTION));
  return _aidl_status;
}
::ndk::ScopedAStatus IRadioImsIndicationDefault::getInterfaceVersion(int32_t* _aidl_return) {
  *_aidl_return = 0;
  return ::ndk::ScopedAStatus(AStatus_newOk());
}
::ndk::SpAIBinder IRadioImsIndicationDefault::asBinder() {
  return ::ndk::SpAIBinder();
}
bool IRadioImsIndicationDefault::isRemote() {
  return false;
}
}  // namespace ims
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
