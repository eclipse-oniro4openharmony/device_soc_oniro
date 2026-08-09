/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * W5: client-side proxy for IWaydroidSession (see waydroid_session.h).
 * Kept free of wl_server.h / the Server class so the front-end app and
 * the bring-up test can link it without the wayland/libhybris deps.
 */

#include "waydroid_session.h"

#include <iservice_registry.h>
#include <message_option.h>
#include <message_parcel.h>
#include <system_ability_definition.h>

namespace OHOS {
namespace Waydroid {

/* ---- proxy -------------------------------------------------------------- */


sptr<IWaydroidSession> WaydroidSessionProxy::Get()
{
    auto samgr = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgr == nullptr) {
        return nullptr;
    }
    sptr<IRemoteObject> obj = samgr->GetSystemAbility(WAYDROID_SESSION_SA_ID);
    if (obj == nullptr) {
        return nullptr;
    }
    return iface_cast<IWaydroidSession>(obj);
}

int32_t WaydroidSessionProxy::SetOutputSurface(const sptr<IRemoteObject>& producer)
{
    MessageParcel data;
    MessageParcel reply;
    MessageOption option;
    if (!data.WriteInterfaceToken(GetDescriptor()) ||
        !data.WriteRemoteObject(producer)) {
        return -1;
    }
    int err = Remote()->SendRequest(SET_OUTPUT_SURFACE, data, reply, option);
    return err == ERR_NONE ? reply.ReadInt32() : err;
}

int32_t WaydroidSessionProxy::ClearOutputSurface()
{
    MessageParcel data;
    MessageParcel reply;
    MessageOption option;
    if (!data.WriteInterfaceToken(GetDescriptor())) {
        return -1;
    }
    int err = Remote()->SendRequest(CLEAR_OUTPUT_SURFACE, data, reply, option);
    return err == ERR_NONE ? reply.ReadInt32() : err;
}

int32_t WaydroidSessionProxy::InjectTouch(int32_t action, int32_t id, int32_t x, int32_t y)
{
    MessageParcel data;
    MessageParcel reply;
    MessageOption option(MessageOption::TF_ASYNC);
    if (!data.WriteInterfaceToken(GetDescriptor()) ||
        !data.WriteInt32(action) || !data.WriteInt32(id) ||
        !data.WriteInt32(x) || !data.WriteInt32(y)) {
        return -1;
    }
    return Remote()->SendRequest(INJECT_TOUCH, data, reply, option);
}

int32_t WaydroidSessionProxy::SetForeground(bool foreground)
{
    MessageParcel data;
    MessageParcel reply;
    MessageOption option;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteBool(foreground)) {
        return -1;
    }
    int err = Remote()->SendRequest(SET_FOREGROUND, data, reply, option);
    return err == ERR_NONE ? reply.ReadInt32() : err;
}

} // namespace Waydroid
} // namespace OHOS
