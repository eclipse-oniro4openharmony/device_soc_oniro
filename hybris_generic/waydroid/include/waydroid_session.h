/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * W5: the compositor's IPC face to the "Android Apps" ArkUI front-end.
 *
 * The compositor is an init-started native root service (sandbox:0) — it
 * must not be an app process, because its per-frame gralloc import needs
 * the /android/vendor mount that appspawn sandboxes lack.  So the app and
 * the compositor are necessarily separate processes and the app's
 * fullscreen XComponent surface reaches the frame path over binder:
 *
 *   app (hap)                         compositor (native, this SA)
 *   ─────────                         ────────────────────────────
 *   XComponent SURFACE
 *     → OHNativeWindow
 *     → surface->GetProducer()
 *     → producer->AsObject()  ──IPC──▶ SetOutputSurface()
 *                                        → Output().AttachProducer()
 *   onTouch  ─────────────────IPC──▶ InjectTouch() → wl_touch
 *   onForeground/onBackground ─IPC──▶ SetForeground() → cgroup freezer
 *
 * selinux is disabled on this image, so the compositor registers this SA
 * at runtime with a free vendor SAID and no sa_profile/policy is needed
 * (samgr's CanRequest passes for a native process; the selinux add-check
 * is skipped).  The app resolves it through GetSystemAbility.
 */

#ifndef WAYDROID_SESSION_H
#define WAYDROID_SESSION_H

#include <iremote_broker.h>
#include <iremote_proxy.h>
#include <iremote_stub.h>

namespace OHOS {
namespace Waydroid {

class Server;

/* Free SAID (1..0x00ffffff), not present in system_ability_definition.h
 * nor the product's sa_install_info.json. */
constexpr int32_t WAYDROID_SESSION_SA_ID = 9601;

class IWaydroidSession : public IRemoteBroker {
public:
    DECLARE_INTERFACE_DESCRIPTOR(u"ohos.waydroid.IWaydroidSession");

    enum Code : uint32_t {
        SET_OUTPUT_SURFACE = 1,   /* in: remote object (IBufferProducer) */
        CLEAR_OUTPUT_SURFACE,     /* revert to the self-drawing node */
        INJECT_TOUCH,             /* in: action, id, x, y */
        SET_FOREGROUND,           /* in: bool (unfreeze/freeze container) */
    };

    /* Touch action matches OHOS PointerEvent action codes so the app can
     * forward them verbatim. */
    virtual int32_t SetOutputSurface(const sptr<IRemoteObject>& producer) = 0;
    virtual int32_t ClearOutputSurface() = 0;
    virtual int32_t InjectTouch(int32_t action, int32_t id, int32_t x, int32_t y) = 0;
    virtual int32_t SetForeground(bool foreground) = 0;
};

/* Hosted by the compositor. */
class WaydroidSessionStub : public IRemoteStub<IWaydroidSession> {
public:
    explicit WaydroidSessionStub(Server* server) : server_(server) {}

    int OnRemoteRequest(uint32_t code, MessageParcel& data, MessageParcel& reply,
                        MessageOption& option) override;

    int32_t SetOutputSurface(const sptr<IRemoteObject>& producer) override;
    int32_t ClearOutputSurface() override;
    int32_t InjectTouch(int32_t action, int32_t id, int32_t x, int32_t y) override;
    int32_t SetForeground(bool foreground) override;

    /* Publish/withdraw the SA with samgr. */
    static bool Publish(Server* server);
    static void Withdraw();

private:
    Server* server_;
};

/* Client side — used by the "Android Apps" front-end (and the bring-up
 * test tool) to reach the compositor. */
class WaydroidSessionProxy : public IRemoteProxy<IWaydroidSession> {
public:
    explicit WaydroidSessionProxy(const sptr<IRemoteObject>& impl)
        : IRemoteProxy<IWaydroidSession>(impl) {}

    int32_t SetOutputSurface(const sptr<IRemoteObject>& producer) override;
    int32_t ClearOutputSurface() override;
    int32_t InjectTouch(int32_t action, int32_t id, int32_t x, int32_t y) override;
    int32_t SetForeground(bool foreground) override;

    /* GetSystemAbility(WAYDROID_SESSION_SA_ID) + cast; nullptr if the
     * compositor is not up. */
    static sptr<IWaydroidSession> Get();

private:
    static inline BrokerDelegator<WaydroidSessionProxy> delegator_;
};

} // namespace Waydroid
} // namespace OHOS

#endif // WAYDROID_SESSION_H
