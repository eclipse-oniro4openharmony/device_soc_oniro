#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <android/binder_interface_utils.h>
#include <aidl/android/hardware/radio/modem/DeviceStateType.h>
#include <aidl/android/hardware/radio/modem/IRadioModemIndication.h>
#include <aidl/android/hardware/radio/modem/IRadioModemResponse.h>
#include <aidl/android/hardware/radio/modem/NvItem.h>
#include <aidl/android/hardware/radio/modem/NvWriteItem.h>
#include <aidl/android/hardware/radio/modem/RadioCapability.h>
#include <aidl/android/hardware/radio/modem/ResetNvType.h>
#ifdef BINDER_STABILITY_SUPPORT
#include <android/binder_stability.h>
#endif  // BINDER_STABILITY_SUPPORT

namespace aidl::android::hardware::radio::modem {
class IRadioModemIndication;
class IRadioModemResponse;
class NvWriteItem;
class RadioCapability;
}  // namespace aidl::android::hardware::radio::modem
namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace modem {
class IRadioModemDelegator;

class IRadioModem : public ::ndk::ICInterface {
public:
  typedef IRadioModemDelegator DefaultDelegator;
  static const char* descriptor;
  IRadioModem();
  virtual ~IRadioModem();

  static const int32_t version = 2;
  static constexpr uint32_t TRANSACTION_enableModem = FIRST_CALL_TRANSACTION + 0;
  static constexpr uint32_t TRANSACTION_getBasebandVersion = FIRST_CALL_TRANSACTION + 1;
  static constexpr uint32_t TRANSACTION_getDeviceIdentity = FIRST_CALL_TRANSACTION + 2;
  static constexpr uint32_t TRANSACTION_getHardwareConfig = FIRST_CALL_TRANSACTION + 3;
  static constexpr uint32_t TRANSACTION_getModemActivityInfo = FIRST_CALL_TRANSACTION + 4;
  static constexpr uint32_t TRANSACTION_getModemStackStatus = FIRST_CALL_TRANSACTION + 5;
  static constexpr uint32_t TRANSACTION_getRadioCapability = FIRST_CALL_TRANSACTION + 6;
  static constexpr uint32_t TRANSACTION_nvReadItem = FIRST_CALL_TRANSACTION + 7;
  static constexpr uint32_t TRANSACTION_nvResetConfig = FIRST_CALL_TRANSACTION + 8;
  static constexpr uint32_t TRANSACTION_nvWriteCdmaPrl = FIRST_CALL_TRANSACTION + 9;
  static constexpr uint32_t TRANSACTION_nvWriteItem = FIRST_CALL_TRANSACTION + 10;
  static constexpr uint32_t TRANSACTION_requestShutdown = FIRST_CALL_TRANSACTION + 11;
  static constexpr uint32_t TRANSACTION_responseAcknowledgement = FIRST_CALL_TRANSACTION + 12;
  static constexpr uint32_t TRANSACTION_sendDeviceState = FIRST_CALL_TRANSACTION + 13;
  static constexpr uint32_t TRANSACTION_setRadioCapability = FIRST_CALL_TRANSACTION + 14;
  static constexpr uint32_t TRANSACTION_setRadioPower = FIRST_CALL_TRANSACTION + 15;
  static constexpr uint32_t TRANSACTION_setResponseFunctions = FIRST_CALL_TRANSACTION + 16;
  static constexpr uint32_t TRANSACTION_getImei = FIRST_CALL_TRANSACTION + 17;

  static std::shared_ptr<IRadioModem> fromBinder(const ::ndk::SpAIBinder& binder);
  static binder_status_t writeToParcel(AParcel* parcel, const std::shared_ptr<IRadioModem>& instance);
  static binder_status_t readFromParcel(const AParcel* parcel, std::shared_ptr<IRadioModem>* instance);
  static bool setDefaultImpl(const std::shared_ptr<IRadioModem>& impl);
  static const std::shared_ptr<IRadioModem>& getDefaultImpl();
  virtual ::ndk::ScopedAStatus enableModem(int32_t in_serial, bool in_on) = 0;
  virtual ::ndk::ScopedAStatus getBasebandVersion(int32_t in_serial) = 0;
  virtual ::ndk::ScopedAStatus getDeviceIdentity(int32_t in_serial) __attribute__((deprecated("use getImei(int serial)"))) = 0;
  virtual ::ndk::ScopedAStatus getHardwareConfig(int32_t in_serial) = 0;
  virtual ::ndk::ScopedAStatus getModemActivityInfo(int32_t in_serial) = 0;
  virtual ::ndk::ScopedAStatus getModemStackStatus(int32_t in_serial) = 0;
  virtual ::ndk::ScopedAStatus getRadioCapability(int32_t in_serial) = 0;
  virtual ::ndk::ScopedAStatus nvReadItem(int32_t in_serial, ::aidl::android::hardware::radio::modem::NvItem in_itemId) __attribute__((deprecated("NV APIs are deprecated starting from Android U."))) = 0;
  virtual ::ndk::ScopedAStatus nvResetConfig(int32_t in_serial, ::aidl::android::hardware::radio::modem::ResetNvType in_resetType) = 0;
  virtual ::ndk::ScopedAStatus nvWriteCdmaPrl(int32_t in_serial, const std::vector<uint8_t>& in_prl) __attribute__((deprecated("NV APIs are deprecated starting from Android U."))) = 0;
  virtual ::ndk::ScopedAStatus nvWriteItem(int32_t in_serial, const ::aidl::android::hardware::radio::modem::NvWriteItem& in_item) __attribute__((deprecated("NV APIs are deprecated starting from Android U."))) = 0;
  virtual ::ndk::ScopedAStatus requestShutdown(int32_t in_serial) = 0;
  virtual ::ndk::ScopedAStatus responseAcknowledgement() = 0;
  virtual ::ndk::ScopedAStatus sendDeviceState(int32_t in_serial, ::aidl::android::hardware::radio::modem::DeviceStateType in_deviceStateType, bool in_state) = 0;
  virtual ::ndk::ScopedAStatus setRadioCapability(int32_t in_serial, const ::aidl::android::hardware::radio::modem::RadioCapability& in_rc) = 0;
  virtual ::ndk::ScopedAStatus setRadioPower(int32_t in_serial, bool in_powerOn, bool in_forEmergencyCall, bool in_preferredForEmergencyCall) = 0;
  virtual ::ndk::ScopedAStatus setResponseFunctions(const std::shared_ptr<::aidl::android::hardware::radio::modem::IRadioModemResponse>& in_radioModemResponse, const std::shared_ptr<::aidl::android::hardware::radio::modem::IRadioModemIndication>& in_radioModemIndication) = 0;
  virtual ::ndk::ScopedAStatus getImei(int32_t in_serial) = 0;
  virtual ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) = 0;
private:
  static std::shared_ptr<IRadioModem> default_impl;
};
class IRadioModemDefault : public IRadioModem {
public:
  ::ndk::ScopedAStatus enableModem(int32_t in_serial, bool in_on) override;
  ::ndk::ScopedAStatus getBasebandVersion(int32_t in_serial) override;
  ::ndk::ScopedAStatus getDeviceIdentity(int32_t in_serial) override __attribute__((deprecated("use getImei(int serial)")));
  ::ndk::ScopedAStatus getHardwareConfig(int32_t in_serial) override;
  ::ndk::ScopedAStatus getModemActivityInfo(int32_t in_serial) override;
  ::ndk::ScopedAStatus getModemStackStatus(int32_t in_serial) override;
  ::ndk::ScopedAStatus getRadioCapability(int32_t in_serial) override;
  ::ndk::ScopedAStatus nvReadItem(int32_t in_serial, ::aidl::android::hardware::radio::modem::NvItem in_itemId) override __attribute__((deprecated("NV APIs are deprecated starting from Android U.")));
  ::ndk::ScopedAStatus nvResetConfig(int32_t in_serial, ::aidl::android::hardware::radio::modem::ResetNvType in_resetType) override;
  ::ndk::ScopedAStatus nvWriteCdmaPrl(int32_t in_serial, const std::vector<uint8_t>& in_prl) override __attribute__((deprecated("NV APIs are deprecated starting from Android U.")));
  ::ndk::ScopedAStatus nvWriteItem(int32_t in_serial, const ::aidl::android::hardware::radio::modem::NvWriteItem& in_item) override __attribute__((deprecated("NV APIs are deprecated starting from Android U.")));
  ::ndk::ScopedAStatus requestShutdown(int32_t in_serial) override;
  ::ndk::ScopedAStatus responseAcknowledgement() override;
  ::ndk::ScopedAStatus sendDeviceState(int32_t in_serial, ::aidl::android::hardware::radio::modem::DeviceStateType in_deviceStateType, bool in_state) override;
  ::ndk::ScopedAStatus setRadioCapability(int32_t in_serial, const ::aidl::android::hardware::radio::modem::RadioCapability& in_rc) override;
  ::ndk::ScopedAStatus setRadioPower(int32_t in_serial, bool in_powerOn, bool in_forEmergencyCall, bool in_preferredForEmergencyCall) override;
  ::ndk::ScopedAStatus setResponseFunctions(const std::shared_ptr<::aidl::android::hardware::radio::modem::IRadioModemResponse>& in_radioModemResponse, const std::shared_ptr<::aidl::android::hardware::radio::modem::IRadioModemIndication>& in_radioModemIndication) override;
  ::ndk::ScopedAStatus getImei(int32_t in_serial) override;
  ::ndk::ScopedAStatus getInterfaceVersion(int32_t* _aidl_return) override;
  ::ndk::SpAIBinder asBinder() override;
  bool isRemote() override;
};
}  // namespace modem
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl
