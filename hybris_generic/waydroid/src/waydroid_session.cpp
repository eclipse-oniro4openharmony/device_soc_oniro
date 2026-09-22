/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compositor-side IPC (see waydroid_session.h): the caller gate, the v1
 * output calls, the v2 window calls, the control plane over libwdbinder,
 * visibility/freezer, and the task events for the shell.
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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>

#include <accesstoken_kit.h>
#include <hilog/log.h>
#include <parameter.h>
#include <if_system_ability_manager.h>
#include <ipc_skeleton.h>
#include <iservice_registry.h>
#include <system_ability_definition.h>

#include <surface.h>
#include <ibuffer_producer.h>

#include "common_event_data.h"
#include "common_event_manager.h"
#include "common_event_publish_info.h"
#include "want.h"

#include "wdbinder.h"
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

/* Visibility lease (see CheckVisibilityLease).  The front-end's heartbeat is
 * 2 s; five missed beats is a dead front-end, not a busy one. */
constexpr int64_t kVisibilityLeaseMs = 10 * 1000;
std::mutex g_applyMutex;
bool g_visible = false;             /* guarded by g_applyMutex */
std::atomic<bool> g_thawed { false };   /* mirror of g_visible for lock-free readers */

/* The shell: the only HAP allowed to call us, the only receiver of our task
 * events. */
constexpr const char* kShellBundle = "org.oniroproject.androidapps";
constexpr const char* kEventTask = "org.oniroproject.waydroid.TASK";
constexpr const char* kVisibleParam = "waydroid.session.visible";
constexpr const char* kOverlayParam = "waydroid.debug.overlay";

/* Deadlines for calls into the container (libwdbinder enforces them). */
constexpr int kQuickMs = 1000;
constexpr int kListMs = 3000;
constexpr int kLaunchMs = 5000;
constexpr size_t kMaxIconBytes = 1024 * 1024;
std::atomic<int64_t> g_lastShowMs { 0 };
std::atomic<int64_t> g_holdHiddenUntilMs { 0 };

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

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

/* The debug overlay: the fullscreen self-drawing node over everything, the
 * global touch grab and the exit chord — how Android was shown before it
 * moved into OHOS windows.  Off unless asked for. */
bool OverlayEnabled()
{
    char v[8] = { 0 };
    GetParameter(kOverlayParam, "0", v, sizeof v);
    return strcmp(v, "1") == 0;
}

/* ---- control plane ------------------------------------------------------------ */

/*
 * One binder connection into the container, shared by the IPC threads.  The
 * node is the container's own /dev/binder reached through its root, so it
 * follows container.pid: a new generation is a new path, and wdb_set_path
 * drops the old connection.
 */
std::mutex g_planeMutex;
struct wdb* g_plane = nullptr;          /* guarded by g_planeMutex */
std::atomic<pid_t> g_readyFor { -1 };         /* generation known to be booted */
std::atomic<pid_t> g_provisionedFor { -1 };   /* generation we provisioned */

int32_t FromWdb(int rc)
{
    switch (rc) {
        case WDB_OK: return SESSION_OK;
        case WDB_EOPEN:
        case WDB_ENOSERVICE: return SESSION_ENOTREADY;
        case WDB_ETIMEDOUT: return SESSION_ETIMEDOUT;
        default: return SESSION_EGENERIC;
    }
}

/* Runs fn(plane) with the connection pointed at the current generation.
 * Never transacts into a frozen container: the call would sit in the target's
 * queue until the deadline and tell us nothing. */
template <typename Fn>
int32_t WithPlane(pid_t* generation, Fn fn)
{
    if (!g_thawed.load()) {
        return SESSION_EFROZEN;
    }
    pid_t init = ReadContainerPid();
    if (init <= 0) {
        return SESSION_ENOTREADY;
    }
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/root/dev/binder", static_cast<long>(init));
    std::lock_guard<std::mutex> lock(g_planeMutex);
    if (g_plane == nullptr) {
        g_plane = wdb_new(path);
        if (g_plane == nullptr) {
            return SESSION_EGENERIC;
        }
    }
    wdb_set_path(g_plane, path);
    if (generation != nullptr) {
        *generation = init;
    }
    return fn(g_plane);
}

bool ValidPackageName(const std::string& package)
{
    if (package.empty() || package.size() > 255) {
        return false;
    }
    for (char c : package) {
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_')) {
            return false;
        }
    }
    return true;
}

/* ---- events for the shell --------------------------------------------------------- */

/* CES publish is an IPC; keep it off the wayland thread. */
std::mutex g_eventMutex;
std::condition_variable g_eventCv;
std::deque<std::string> g_events;
std::thread g_eventThread;
std::thread g_planeThread;
std::atomic<bool> g_workersRun { false };

void PublishTaskEvent(const std::string& data)
{
    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        g_events.push_back(data);
    }
    g_eventCv.notify_one();
}

void EventWorker()
{
    while (g_workersRun.load()) {
        std::string data;
        {
            std::unique_lock<std::mutex> lock(g_eventMutex);
            g_eventCv.wait_for(lock, std::chrono::milliseconds(500),
                               [] { return !g_events.empty() || !g_workersRun.load(); });
            if (g_events.empty()) {
                continue;
            }
            data = g_events.front();
            g_events.pop_front();
        }
        AAFwk::Want want;
        want.SetAction(kEventTask);
        EventFwk::CommonEventData event(want);
        event.SetData(data);
        EventFwk::CommonEventPublishInfo info;
        info.SetBundleName(kShellBundle);       /* nobody else gets to watch */
        if (!EventFwk::CommonEventManager::PublishCommonEvent(event, info)) {
            HILOG_WARN(LOG_CORE, "publish TASK '%{public}s' failed", data.c_str());
        }
    }
}

/*
 * Once per generation, as soon as Android answers and while it is thawed:
 *
 *  - mark it provisioned.  An Android that thinks it is fresh out of the box
 *    puts its setup wizard in front of whatever is launched;
 *  - take screen power away from it.  OHOS owns the panel and freezes the
 *    container when nothing of it is shown; Android's own 60 s timeout would
 *    put its display to sleep under a window the user is looking at.
 */
void PlaneWorker()
{
    while (g_workersRun.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        pid_t generation = -1;
        int32_t rc = WithPlane(&generation, [&generation](struct wdb* b) -> int32_t {
            if (g_provisionedFor.load() == generation) {
                return 1;
            }
            char* booted = nullptr;
            int r = wdb_platform_getprop(b, "sys.boot_completed", "0", &booted, kQuickMs);
            bool up = (r == WDB_OK && booted != nullptr && booted[0] == '1');
            free(booted);
            if (!up) {
                return FromWdb(r == WDB_OK ? WDB_ENOSERVICE : r);
            }
            g_readyFor.store(generation);
            r = wdb_platform_settings_put_int(b, WDB_SETTINGS_SECURE, "user_setup_complete", 1,
                                              kQuickMs);
            if (r == WDB_OK) {
                r = wdb_platform_settings_put_int(b, WDB_SETTINGS_GLOBAL, "device_provisioned", 1,
                                                  kQuickMs);
            }
            if (r == WDB_OK) {
                r = wdb_platform_settings_put_int(b, WDB_SETTINGS_SYSTEM, "screen_off_timeout",
                                                  INT32_MAX, kQuickMs);
            }
            if (r == WDB_OK) {
                g_provisionedFor.store(generation);
            }
            return FromWdb(r);
        });
        if (rc == SESSION_OK) {
            HILOG_INFO(LOG_CORE, "Android is up (generation %{public}ld): provisioned, "
                       "screen timeout disabled", static_cast<long>(generation));
            PublishTaskEvent("ready");
        }
    }
}

} // namespace

int WaydroidSessionStub::OnRemoteRequest(uint32_t code, MessageParcel& data,
                                         MessageParcel& reply, MessageOption& option)
{
    if (data.ReadInterfaceToken() != GetDescriptor()) {
        HILOG_ERROR(LOG_CORE, "bad interface token");
        return ERR_INVALID_STATE;
    }
    /* The gate (see the header): the shell bundle, or root. */
    {
        using namespace Security::AccessToken;
        const AccessTokenID token = IPCSkeleton::GetCallingTokenID();
        bool allowed = false;
        if (AccessTokenKit::GetTokenTypeFlag(token) == TOKEN_HAP) {
            HapTokenInfo hap;
            allowed = AccessTokenKit::GetHapTokenInfo(token, hap) == 0 &&
                      hap.bundleName == kShellBundle;
        } else {
            allowed = IPCSkeleton::GetCallingUid() == 0;
        }
        if (!allowed) {
            HILOG_WARN(LOG_CORE, "request %{public}u refused: caller uid %{public}d is not the shell",
                       code, IPCSkeleton::GetCallingUid());
            reply.WriteInt32(SESSION_EDENIED);
            return ERR_NONE;
        }
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
        case GET_STATE:
            reply.WriteInt32(GetState());
            return ERR_NONE;
        case LIST_APPS: {
            std::vector<SessionApp> apps;
            int32_t rc = ListApps(apps);
            reply.WriteInt32(rc);
            if (rc == SESSION_OK) {
                reply.WriteInt32(static_cast<int32_t>(apps.size()));
                for (const SessionApp& app : apps) {
                    reply.WriteString(app.name);
                    reply.WriteString(app.package);
                }
            }
            return ERR_NONE;
        }
        case GET_APP_ICON: {
            std::vector<uint8_t> png;
            int32_t rc = GetAppIcon(data.ReadString(), png);
            reply.WriteInt32(rc);
            if (rc == SESSION_OK) {
                reply.WriteUInt8Vector(png);
            }
            return ERR_NONE;
        }
        case LAUNCH_APP:
            reply.WriteInt32(LaunchApp(data.ReadString()));
            return ERR_NONE;
        case CLOSE_APP:
            reply.WriteInt32(CloseApp(data.ReadString()));
            return ERR_NONE;
        case ATTACH_WINDOW: {
            std::string package = data.ReadString();
            sptr<IRemoteObject> obj = data.ReadRemoteObject();
            int32_t w = data.ReadInt32();
            int32_t h = data.ReadInt32();
            reply.WriteInt32(AttachWindow(package, obj, w, h));
            return ERR_NONE;
        }
        case DETACH_WINDOW:
            reply.WriteInt32(DetachWindow(data.ReadInt32()));
            return ERR_NONE;
        case WINDOW_TOUCH: {
            int32_t window = data.ReadInt32();
            int32_t action = data.ReadInt32();
            int32_t id = data.ReadInt32();
            int32_t x = data.ReadInt32();
            int32_t y = data.ReadInt32();
            reply.WriteInt32(WindowTouch(window, action, id, x, y));
            return ERR_NONE;
        }
        case WINDOW_KEY: {
            int32_t window = data.ReadInt32();
            int32_t key = data.ReadInt32();
            bool down = data.ReadBool();
            reply.WriteInt32(WindowKey(window, key, down));
            return ERR_NONE;
        }
        case SET_WINDOW_ACTIVE: {
            int32_t window = data.ReadInt32();
            bool active = data.ReadBool();
            reply.WriteInt32(SetWindowActive(window, active));
            return ERR_NONE;
        }
        case WINDOW_ALIVE:
            reply.WriteInt32(WindowAlive(data.ReadInt32()));
            return ERR_NONE;
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
    /* Back to the overlay's node only if the overlay is what is wanted;
     * otherwise just stop showing — re-creating a fullscreen Z=100000 node
     * over OHOS because a client went away is how Android ended up painted
     * over the lock screen. */
    if (OverlayEnabled()) {
        server_->RevertToSelfDrawing();
    } else {
        server_->Output().Detach();
        server_->ReleaseInFlightOf(0);
    }
    HILOG_INFO(LOG_CORE, "ClearOutputSurface: legacy output released");
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
    g_thawed.store(foreground);
    return 0;
}

int32_t WaydroidSessionStub::GetState()
{
    if (!g_thawed.load()) {
        return SESSION_STATE_FROZEN;
    }
    pid_t init = ReadContainerPid();
    return (init > 0 && g_readyFor.load() == init) ? SESSION_STATE_READY : SESSION_STATE_BOOTING;
}

int32_t WaydroidSessionStub::ListApps(std::vector<SessionApp>& apps)
{
    return WithPlane(nullptr, [&apps](struct wdb* b) -> int32_t {
        struct wdb_app* list = nullptr;
        size_t n = 0;
        int rc = wdb_platform_list_apps(b, &list, &n, kListMs);
        if (rc != WDB_OK) {
            HILOG_WARN(LOG_CORE, "ListApps: %{public}s", wdb_strerror(rc));
            return FromWdb(rc);
        }
        for (size_t i = 0; i < n; i++) {
            SessionApp app;
            app.name = list[i].name != nullptr ? list[i].name : list[i].package;
            app.package = list[i].package;
            apps.push_back(app);
        }
        wdb_platform_free_apps(list, n);
        return SESSION_OK;
    });
}

/* Android exports one PNG per launcher app to /data/icons at boot.  /data is
 * the container's loop-mounted data.img, so we read it through the
 * container's root — no process of the container is involved, so this works
 * frozen too. */
int32_t WaydroidSessionStub::GetAppIcon(const std::string& package, std::vector<uint8_t>& png)
{
    if (!ValidPackageName(package)) {
        return SESSION_EINVAL;
    }
    pid_t init = ReadContainerPid();
    if (init <= 0) {
        return SESSION_ENOTREADY;
    }
    char path[512];
    snprintf(path, sizeof path, "/proc/%ld/root/data/icons/%s.png", static_cast<long>(init),
             package.c_str());
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return SESSION_ENOTREADY;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        static_cast<size_t>(st.st_size) > kMaxIconBytes) {
        close(fd);
        return SESSION_EGENERIC;
    }
    png.resize(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < png.size()) {
        ssize_t r = read(fd, png.data() + got, png.size() - got);
        if (r <= 0) {
            break;
        }
        got += static_cast<size_t>(r);
    }
    close(fd);
    if (got != png.size()) {
        png.clear();
        return SESSION_EGENERIC;
    }
    return SESSION_OK;
}

int32_t WaydroidSessionStub::LaunchApp(const std::string& package)
{
    if (!ValidPackageName(package)) {
        return SESSION_EINVAL;
    }
    int32_t rc = WithPlane(nullptr, [&package](struct wdb* b) -> int32_t {
        /* The hwc shows one window per Android task — and that is what the
         * frame routing relies on — whenever waydroid.active_apps is not
         * "Waydroid" (the whole desktop in one window, its boot value).
         * launchApp moves the property along by itself once it is in that
         * mode, but does not leave "Waydroid" on its own. */
        int r = wdb_platform_setprop(b, "waydroid.active_apps", package.c_str(), kQuickMs);
        if (r != WDB_OK) {
            return FromWdb(r);
        }
        return FromWdb(wdb_platform_launch_app(b, package.c_str(), kLaunchMs));
    });
    HILOG_INFO(LOG_CORE, "LaunchApp %{public}s -> %{public}d", package.c_str(), rc);
    return rc;
}

int32_t WaydroidSessionStub::CloseApp(const std::string& package)
{
    if (server_ == nullptr || !ValidPackageName(package)) {
        return SESSION_EINVAL;
    }
    server_->CloseApp(package);
    return SESSION_OK;
}

int32_t WaydroidSessionStub::AttachWindow(const std::string& package,
                                          const sptr<IRemoteObject>& producerObj,
                                          int32_t width, int32_t height)
{
    if (server_ == nullptr || producerObj == nullptr ||
        (!package.empty() && !ValidPackageName(package))) {
        return SESSION_EINVAL;
    }
    sptr<IBufferProducer> producer = iface_cast<IBufferProducer>(producerObj);
    if (producer == nullptr) {
        HILOG_ERROR(LOG_CORE, "AttachWindow: not an IBufferProducer");
        return SESSION_EINVAL;
    }
    int32_t id = server_->AttachWindow(package, producer, width, height);
    return id > 0 ? id : SESSION_EGENERIC;
}

int32_t WaydroidSessionStub::DetachWindow(int32_t window)
{
    return (server_ != nullptr && server_->DetachWindow(window)) ? SESSION_OK : SESSION_ENOWINDOW;
}

int32_t WaydroidSessionStub::WindowTouch(int32_t window, int32_t action, int32_t id, int32_t x,
                                         int32_t y)
{
    if (server_ == nullptr) {
        return SESSION_EGENERIC;
    }
    server_->WindowTouch(window, action, id, x, y);
    return SESSION_OK;
}

int32_t WaydroidSessionStub::WindowKey(int32_t window, int32_t code, bool down)
{
    if (server_ == nullptr || code <= 0 || code > 0x2ff) {
        return SESSION_EINVAL;
    }
    server_->WindowKey(window, code, down);
    return SESSION_OK;
}

int32_t WaydroidSessionStub::SetWindowActive(int32_t window, bool active)
{
    if (server_ == nullptr) {
        return SESSION_EGENERIC;
    }
    server_->SetWindowActive(window, active);
    return SESSION_OK;
}

int32_t WaydroidSessionStub::WindowAlive(int32_t window)
{
    return (server_ != nullptr && server_->WindowAlive(window)) ? 1 : 0;
}

/* Wayland thread.  The shell's view of the toplevel table:
 *   ontop <package>    that task's frames are what Android presents now
 *   inactive <package> that task stopped being presented: the user backed out
 *                      of its last activity, or another task took over
 *   home               Android's HOME got a window and came to the top (only
 *                      when the hwc gives HOME one; see IsHomePackage)
 *   gone <package>     its window was destroyed (task ended, or we closed it)
 *   allgone            no task window is left
 * The full-UI toplevel ("Waydroid") has no package and is reported as "*". */
void WaydroidSessionStub::OnToplevelEvent(Server* server, ToplevelEvent ev,
                                          const ToplevelInfo& info)
{
    constexpr const char* prefix = "waydroid.";
    std::string package = info.appId.compare(0, strlen(prefix), prefix) == 0
        ? info.appId.substr(strlen(prefix)) : std::string("*");
    if (IsHomePackage(package)) {
        if (ev == ToplevelEvent::OnTop) {
            PublishTaskEvent("home");
        }
        return;
    }
    if (ev == ToplevelEvent::OnTop) {
        PublishTaskEvent("ontop " + package);
    } else if (ev == ToplevelEvent::Inactive) {
        PublishTaskEvent("inactive " + package);
    } else if (ev == ToplevelEvent::Destroyed) {
        PublishTaskEvent("gone " + package);
        if (server != nullptr && server->Toplevels().empty()) {
            PublishTaskEvent("allgone");
        }
    }
}

void WaydroidSessionStub::StartControlPlane(Server*)
{
    if (g_workersRun.exchange(true)) {
        return;
    }
    g_eventThread = std::thread(EventWorker);
    g_planeThread = std::thread(PlaneWorker);
}

void WaydroidSessionStub::StopControlPlane()
{
    if (!g_workersRun.exchange(false)) {
        return;
    }
    g_eventCv.notify_all();
    if (g_eventThread.joinable()) {
        g_eventThread.join();
    }
    if (g_planeThread.joinable()) {
        g_planeThread.join();
    }
    std::lock_guard<std::mutex> lock(g_planeMutex);
    wdb_free(g_plane);
    g_plane = nullptr;
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
    std::lock_guard<std::mutex> lock(g_applyMutex);
    if (visible) {
        if (NowMs() < g_holdHiddenUntilMs.load()) {
            return;     /* exit chord in progress (HideAndHold) */
        }
        g_lastShowMs.store(NowMs());
        if (g_visible) {
            return;     /* heartbeat: lease renewed, nothing to redo */
        }
        g_visible = true;
        if (OverlayEnabled()) {
            /* Make the node visible (it keeps its last frame, so the container
             * reappears instantly).  If the node was never attached — e.g.
             * after an app-producer path dropped it — recreate it. */
            if (server->Output().IsAttached()) {
                /* IsAttached() only proves we hold a Surface, NOT that RS put
                 * our node on the render tree — the startup attach is normally
                 * dropped, and an off-tree node draws nothing however visible we
                 * make it.  Heal that first; it is a one-shot per node and a
                 * no-op thereafter (OutputSurface::EnsureAttachedToDisplay). */
                server->Output().EnsureAttachedToDisplay();
                server->Output().SetNodeVisible(true);
            } else {
                server->RevertToSelfDrawing();
            }
        }
        FreezeContainer(false);
        g_thawed.store(true);
        SetParameter(kVisibleParam, "1");
        if (OverlayEnabled()) {
            /* Touch follows visibility: grab the touchscreen for the container
             * while it is shown (GrabController watches this param).  When
             * hidden, leaving the grab on would swallow all touch and make
             * OHOS unusable. */
            SetParameter("waydroid.input.grab", "1");
        }
        HILOG_INFO(LOG_CORE, "visible: container thawed%{public}s",
                   OverlayEnabled() ? " + overlay shown + touch grabbed" : "");
    } else {
        g_visible = false;
        /* Freeze (stop producing).  Overlay: hide the node WITHOUT destroying
         * it (its last frame survives for the next show) and release touch. */
        g_thawed.store(false);
        FreezeContainer(true);
        SetParameter(kVisibleParam, "0");
        server->Output().SetNodeVisible(false);
        SetParameter("waydroid.input.grab", "0");
        HILOG_INFO(LOG_CORE, "hidden: container frozen");
    }
}

void WaydroidSessionStub::HideAndHold(Server* server, int64_t holdMs)
{
    g_holdHiddenUntilMs.store(NowMs() + holdMs);
    ApplyVisibility(server, false);
}

void WaydroidSessionStub::CheckVisibilityLease(Server* server)
{
    {
        std::lock_guard<std::mutex> lock(g_applyMutex);
        if (!g_visible || NowMs() - g_lastShowMs.load() < kVisibilityLeaseMs) {
            return;
        }
    }
    HILOG_WARN(LOG_CORE, "no SHOW heartbeat for %{public}lld ms — front-end gone; hiding",
               static_cast<long long>(kVisibilityLeaseMs));
    ApplyVisibility(server, false);
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
