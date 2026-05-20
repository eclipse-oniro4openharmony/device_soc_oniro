/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * HwServiceManager — convenience wrapper over the well-known handle 0
 * on /dev/hwbinder (Halium's hwservicemanager).
 *
 * Exposes:
 *   - Ping(interfaceDescriptor)     : IBase::ping smoke
 *   - GetService(fqName, instance,  : IServiceManager::get(fqName, name)
 *                &outHandle)          handle resolution
 *
 * IServiceManager transaction codes are the standard hidl-gen
 * FIRST_CALL_TRANSACTION (1) + N pattern (NOT the 0x0F* mnemonics
 * used by IBase methods).  Codes 1..8 in declaration order:
 *   1 get, 2 add, 3 getTransport, 4 list, 5 listByInterface,
 *   6 registerForNotifications, 7 debugDump, 8 registerPassthroughClient
 * Confirmed by disassembling BnHwServiceManager::onTransact +
 * BpHwServiceManager::_hidl_get in libhidlbase.so on the X23
 * (2026-05-21).
 */

#ifndef HYBRIS_HIDL_HW_SERVICE_MANAGER_H
#define HYBRIS_HIDL_HW_SERVICE_MANAGER_H

#include <string>

#include "hw_binder_client.h"

namespace OHOS::Camera::Hybris::Hidl {

class HwServiceManager {
public:
    explicit HwServiceManager(HwBinderClient *client) : client_(client) {}

    /*
     * Send IBase::ping to handle 0 with the supplied interface
     * descriptor as the enforceInterface token.  Returns true if the
     * receiver echoed back EX_NONE (status=0) — i.e. our wire format
     * is valid and the service is alive.
     *
     * Default descriptor is "android.hidl.base@1.0::IBase" — the
     * base-class descriptor that IBase::_hidl_ping always checks for,
     * regardless of the concrete service.  Validated by disassembly.
     */
    bool Ping(const char *interfaceDescriptor = "android.hidl.base@1.0::IBase");

    /*
     * Resolve a HIDL service name to a strong binder handle.
     * Equivalent to libhwbinder's IServiceManager::get(fqName, name).
     *
     * `fqName` is the fully-qualified interface name with version,
     *   e.g. "android.hardware.camera.provider@2.6::ICameraProvider".
     * `instance` is the registered instance name,
     *   e.g. "legacy/0" or "default".
     *
     * Returns true on success with `*outHandle` set to a numeric
     * binder handle (still needs Acquire() before use).  Returns
     * false if hwservicemanager returns null, an exception, or
     * the transaction fails.  `*outIsNull` is set true when the
     * service exists but the registry has no entry under that name.
     */
    bool GetService(const std::string &fqName, const std::string &instance,
                    uint32_t *outHandle, bool *outIsNull);

    /* For future use: a strongly-held ref on hwservicemanager itself. */
    HwBinderClient::StrongHandle &SmHandle() { return smHandle_; }
    bool EnsureAcquired();

    /*
     * IServiceManager transaction codes — wire constants for
     * hwservicemanager's onTransact dispatch table.  See class
     * comment above for the source.
     */
    enum : uint32_t {
        TRANSACTION_get                          = 1,
        TRANSACTION_add                          = 2,
        TRANSACTION_getTransport                 = 3,
        TRANSACTION_list                         = 4,
        TRANSACTION_listByInterface              = 5,
        TRANSACTION_registerForNotifications     = 6,
        TRANSACTION_debugDump                    = 7,
        TRANSACTION_registerPassthroughClient    = 8,
    };

    static constexpr const char *kDescriptor =
        "android.hidl.manager@1.0::IServiceManager";

private:
    HwBinderClient *client_;
    HwBinderClient::StrongHandle smHandle_;
};

} // namespace OHOS::Camera::Hybris::Hidl

#endif // HYBRIS_HIDL_HW_SERVICE_MANAGER_H
