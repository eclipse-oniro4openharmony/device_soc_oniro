/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
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
    sptr<IRemoteObject> obj = samgr->CheckSystemAbility(WAYDROID_SESSION_SA_ID);
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

/* ---- v2 --------------------------------------------------------------------- */

namespace {
/* Request with only the token written so far; returns the int32 result or a
 * negative transport error. */
int32_t Call(const sptr<IRemoteObject>& remote, uint32_t code, MessageParcel& data,
             MessageParcel& reply, bool oneway = false)
{
    if (remote == nullptr) {
        return SESSION_EGENERIC;
    }
    MessageOption option(oneway ? MessageOption::TF_ASYNC : MessageOption::TF_SYNC);
    int err = remote->SendRequest(code, data, reply, option);
    if (err != ERR_NONE) {
        return SESSION_EGENERIC;
    }
    return oneway ? SESSION_OK : reply.ReadInt32();
}
} // namespace

int32_t WaydroidSessionProxy::GetState()
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor())) {
        return SESSION_EGENERIC;
    }
    return Call(Remote(), GET_STATE, data, reply);
}

int32_t WaydroidSessionProxy::ListApps(std::vector<SessionApp>& apps)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor())) {
        return SESSION_EGENERIC;
    }
    int32_t rc = Call(Remote(), LIST_APPS, data, reply);
    if (rc != SESSION_OK) {
        return rc;
    }
    int32_t n = reply.ReadInt32();
    for (int32_t i = 0; i < n && i < 4096; i++) {
        SessionApp app;
        app.name = reply.ReadString();
        app.package = reply.ReadString();
        apps.push_back(app);
    }
    return SESSION_OK;
}

int32_t WaydroidSessionProxy::GetAppIcon(const std::string& package, std::vector<uint8_t>& png)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteString(package)) {
        return SESSION_EGENERIC;
    }
    int32_t rc = Call(Remote(), GET_APP_ICON, data, reply);
    if (rc == SESSION_OK && !reply.ReadUInt8Vector(&png)) {
        return SESSION_EGENERIC;
    }
    return rc;
}

int32_t WaydroidSessionProxy::LaunchApp(const std::string& package)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteString(package)) {
        return SESSION_EGENERIC;
    }
    return Call(Remote(), LAUNCH_APP, data, reply);
}

int32_t WaydroidSessionProxy::CloseApp(const std::string& package)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteString(package)) {
        return SESSION_EGENERIC;
    }
    return Call(Remote(), CLOSE_APP, data, reply);
}

int32_t WaydroidSessionProxy::AttachWindow(const std::string& package,
                                           const sptr<IRemoteObject>& producer,
                                           int32_t width, int32_t height)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteString(package) ||
        !data.WriteRemoteObject(producer) || !data.WriteInt32(width) ||
        !data.WriteInt32(height)) {
        return SESSION_EGENERIC;
    }
    return Call(Remote(), ATTACH_WINDOW, data, reply);
}

int32_t WaydroidSessionProxy::DetachWindow(int32_t window)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteInt32(window)) {
        return SESSION_EGENERIC;
    }
    return Call(Remote(), DETACH_WINDOW, data, reply);
}

int32_t WaydroidSessionProxy::WindowTouch(int32_t window, int32_t action, int32_t id, int32_t x,
                                          int32_t y)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteInt32(window) ||
        !data.WriteInt32(action) || !data.WriteInt32(id) || !data.WriteInt32(x) ||
        !data.WriteInt32(y)) {
        return SESSION_EGENERIC;
    }
    /* One-way: touch runs on the UI thread and must never wait for us. */
    return Call(Remote(), WINDOW_TOUCH, data, reply, true);
}

int32_t WaydroidSessionProxy::WindowKey(int32_t window, int32_t code, bool down)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteInt32(window) ||
        !data.WriteInt32(code) || !data.WriteBool(down)) {
        return SESSION_EGENERIC;
    }
    return Call(Remote(), WINDOW_KEY, data, reply, true);
}

int32_t WaydroidSessionProxy::SetWindowActive(int32_t window, bool active)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteInt32(window) ||
        !data.WriteBool(active)) {
        return SESSION_EGENERIC;
    }
    return Call(Remote(), SET_WINDOW_ACTIVE, data, reply);
}

int32_t WaydroidSessionProxy::WindowAlive(int32_t window)
{
    MessageParcel data;
    MessageParcel reply;
    if (!data.WriteInterfaceToken(GetDescriptor()) || !data.WriteInt32(window)) {
        return SESSION_EGENERIC;
    }
    return Call(Remote(), WINDOW_ALIVE, data, reply);
}

} // namespace Waydroid
} // namespace OHOS
