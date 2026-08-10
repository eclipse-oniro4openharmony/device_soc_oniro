/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * W5: compositor-side IPC (see waydroid_session.h).
 */

#include "waydroid_session.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <hilog/log.h>
#include <parameter.h>
#include <if_system_ability_manager.h>
#include <ipc_skeleton.h>
#include <iservice_registry.h>
#include <system_ability_definition.h>

#include <surface.h>
#include <ibuffer_producer.h>

#include "wl_server.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "waydroid_session"

namespace OHOS {
namespace Waydroid {

namespace {

constexpr const char* kContainerPidFile = "/data/waydroid/container.pid";
constexpr const char* kFreezeCgroup = "/sys/fs/cgroup/waydroid";

int WriteFile(const std::string& path, const std::string& value)
{
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = write(fd, value.c_str(), value.size());
    close(fd);
    return n == static_cast<ssize_t>(value.size()) ? 0 : -1;
}

pid_t ReadContainerPid()
{
    FILE* f = fopen(kContainerPidFile, "re");
    if (f == nullptr) {
        return -1;
    }
    long pid = -1;
    if (fscanf(f, "%ld", &pid) != 1) {
        pid = -1;
    }
    fclose(f);
    return static_cast<pid_t>(pid);
}

/* The container was cloned CLONE_NEWPID; the compositor is in the host
 * pid namespace, so every container process appears under /proc with a
 * host pid whose pid-ns inode matches the container init's.  Collect
 * them by that inode. */
ino_t PidNsInode(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/ns/pid", static_cast<long>(pid));
    struct stat st;
    return (stat(path, &st) == 0) ? st.st_ino : 0;
}

std::vector<pid_t> ContainerProcs(pid_t containerInit)
{
    std::vector<pid_t> pids;
    ino_t want = PidNsInode(containerInit);
    if (want == 0) {
        return pids;
    }
    DIR* d = opendir("/proc");
    if (d == nullptr) {
        return pids;
    }
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        pid_t pid = static_cast<pid_t>(atol(e->d_name));
        if (pid > 0 && PidNsInode(pid) == want) {
            pids.push_back(pid);
        }
    }
    closedir(d);
    return pids;
}

/* cgroup v2 freezer.  Moving a pid moves only that thread-group; children
 * created afterwards inherit, but existing ones must be moved too — so we
 * re-scan and move every container process before freezing. */
bool FreezeContainer(bool freeze)
{
    pid_t init = ReadContainerPid();
    if (init <= 0) {
        HILOG_WARN(LOG_CORE, "no container pid — cannot %{public}s",
                   freeze ? "freeze" : "thaw");
        return false;
    }
    if (mkdir(kFreezeCgroup, 0755) < 0 && errno != EEXIST) {
        HILOG_ERROR(LOG_CORE, "mkdir %{public}s: %{public}s", kFreezeCgroup,
                    strerror(errno));
        return false;
    }
    if (freeze) {
        std::string procsPath = std::string(kFreezeCgroup) + "/cgroup.procs";
        for (pid_t pid : ContainerProcs(init)) {
            /* Best-effort: kernel threads and already-exited pids fail. */
            (void)WriteFile(procsPath, std::to_string(pid));
        }
    }
    std::string freezePath = std::string(kFreezeCgroup) + "/cgroup.freeze";
    if (WriteFile(freezePath, freeze ? "1" : "0") < 0) {
        HILOG_ERROR(LOG_CORE, "write %{public}s: %{public}s", freezePath.c_str(),
                    strerror(errno));
        return false;
    }
    HILOG_INFO(LOG_CORE, "container %{public}s", freeze ? "frozen" : "thawed");
    return true;
}

} // namespace

int WaydroidSessionStub::OnRemoteRequest(uint32_t code, MessageParcel& data,
                                         MessageParcel& reply, MessageOption& option)
{
    if (data.ReadInterfaceToken() != GetDescriptor()) {
        HILOG_ERROR(LOG_CORE, "bad interface token");
        return ERR_INVALID_STATE;
    }
    switch (code) {
        case SET_OUTPUT_SURFACE: {
            sptr<IRemoteObject> obj = data.ReadRemoteObject();
            reply.WriteInt32(SetOutputSurface(obj));
            return ERR_NONE;
        }
        case CLEAR_OUTPUT_SURFACE:
            reply.WriteInt32(ClearOutputSurface());
            return ERR_NONE;
        case INJECT_TOUCH: {
            int32_t action = data.ReadInt32();
            int32_t id = data.ReadInt32();
            int32_t x = data.ReadInt32();
            int32_t y = data.ReadInt32();
            reply.WriteInt32(InjectTouch(action, id, x, y));
            return ERR_NONE;
        }
        case SET_FOREGROUND: {
            bool fg = data.ReadBool();
            reply.WriteInt32(SetForeground(fg));
            return ERR_NONE;
        }
        default:
            return IPCObjectStub::OnRemoteRequest(code, data, reply, option);
    }
}

int32_t WaydroidSessionStub::SetOutputSurface(const sptr<IRemoteObject>& producerObj)
{
    if (server_ == nullptr || producerObj == nullptr) {
        return -1;
    }
    sptr<IBufferProducer> producer = iface_cast<IBufferProducer>(producerObj);
    if (producer == nullptr) {
        HILOG_ERROR(LOG_CORE, "SetOutputSurface: not an IBufferProducer");
        return -1;
    }
    bool ok = server_->Output().AttachProducer(producer);
    HILOG_INFO(LOG_CORE, "SetOutputSurface: %{public}s", ok ? "ok" : "failed");
    return ok ? 0 : -1;
}

int32_t WaydroidSessionStub::ClearOutputSurface()
{
    if (server_ == nullptr) {
        return -1;
    }
    server_->RevertToSelfDrawing();
    HILOG_INFO(LOG_CORE, "ClearOutputSurface: reverted to self-drawing node");
    return 0;
}

int32_t WaydroidSessionStub::InjectTouch(int32_t action, int32_t id, int32_t x, int32_t y)
{
    if (server_ == nullptr) {
        return -1;
    }
    server_->InjectTouchFromAction(action, id, x, y);
    return 0;
}

int32_t WaydroidSessionStub::SetForeground(bool foreground)
{
    /* Thaw before showing; freeze after hiding. */
    FreezeContainer(!foreground);
    return 0;
}

void WaydroidSessionStub::ApplyVisibility(Server* server, bool visible)
{
    if (server == nullptr) {
        return;
    }
    /* Serialize: the CES subscriber callback can fire concurrently when
     * visibility flips fast (app fore/background churn). Without this the
     * freeze and grab writes of two calls interleave and can strand grab=1
     * over a frozen container — touch is then stolen from OHOS and dropped
     * by the frozen container (total touch loss). */
    static std::mutex applyMutex;
    std::lock_guard<std::mutex> lock(applyMutex);
    if (visible) {
        /* Make the node visible (it keeps its last frame, so the container
         * reappears instantly) then thaw. If the node was never attached
         * — e.g. after an app-producer path dropped it — recreate it. */
        if (server->Output().IsAttached()) {
            server->Output().SetNodeVisible(true);
        } else {
            server->RevertToSelfDrawing();
        }
        FreezeContainer(false);
        /* Touch follows visibility: grab the touchscreen for the container
         * while it is shown (GrabController watches this param). Without
         * this the container gets no touch; and when hidden, leaving the
         * grab on would swallow all touch and make OHOS unusable. */
        SetParameter("waydroid.input.grab", "1");
        HILOG_INFO(LOG_CORE, "visible: output shown + container thawed + touch grabbed");
    } else {
        /* Freeze (stop producing), hide the node WITHOUT destroying it (so
         * its last frame survives for the next show — a fresh node would be
         * black until the container redraws), release touch to OHOS. */
        FreezeContainer(true);
        server->Output().SetNodeVisible(false);
        SetParameter("waydroid.input.grab", "0");
        HILOG_INFO(LOG_CORE, "hidden: container frozen + output hidden + touch released");
    }
}

bool WaydroidSessionStub::Publish(Server* server)
{
    auto samgr = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgr == nullptr) {
        HILOG_ERROR(LOG_CORE, "no samgr — session SA not published");
        return false;
    }
    sptr<WaydroidSessionStub> stub(new WaydroidSessionStub(server));
    int32_t ret = samgr->AddSystemAbility(WAYDROID_SESSION_SA_ID, stub);
    if (ret != ERR_OK) {
        HILOG_ERROR(LOG_CORE, "AddSystemAbility(%{public}d) failed: %{public}d",
                    WAYDROID_SESSION_SA_ID, ret);
        return false;
    }
    HILOG_INFO(LOG_CORE, "session SA %{public}d published", WAYDROID_SESSION_SA_ID);
    return true;
}

void WaydroidSessionStub::Withdraw()
{
    auto samgr = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgr != nullptr) {
        samgr->RemoveSystemAbility(WAYDROID_SESSION_SA_ID);
    }
}

} // namespace Waydroid
} // namespace OHOS
