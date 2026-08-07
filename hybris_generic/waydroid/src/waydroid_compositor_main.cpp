/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * waydroid_compositor — the OHOS side of the Waydroid display bridge.
 *
 * An init-started native service (root, sandbox:0): it must NOT be an
 * app process, because the gralloc import it does per frame needs the
 * nested /android/vendor mount that appspawn'd sandboxes cannot see.
 *
 * Stage 1 (this file today): output is a self-drawing RSSurfaceNode, so
 * container pixels reach the panel with no ArkUI app in the picture.
 * Stage 2 swaps in an XComponent producer over binder — same frame path.
 */

#include <csignal>
#include <cstdlib>

#include <hilog/log.h>
#include <parameter.h>
#include <transaction/rs_interfaces.h>

#include "wl_server.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "waydroid_compositor"

using namespace OHOS;
using namespace OHOS::Waydroid;

namespace {

Server* g_server = nullptr;

void OnSignal(int sig)
{
    HILOG_INFO(LOG_CORE, "signal %{public}d — stopping", sig);
    if (g_server != nullptr) {
        g_server->Stop();
    }
}

/* Panel geometry from RS; falls back to the ansuz panel if the query
 * fails (which happens only if RS is not up yet — the service is
 * ordered after it, so treat it as a soft error). */
void QueryDisplay(ServerConfig& cfg)
{
    auto& rs = Rosen::RSInterfaces::GetInstance();
    Rosen::ScreenId screenId = rs.GetDefaultScreenId();
    if (screenId == Rosen::INVALID_SCREEN_ID) {
        HILOG_WARN(LOG_CORE, "no default screen — using %{public}dx%{public}d",
                   cfg.width, cfg.height);
        return;
    }
    Rosen::RSScreenModeInfo mode = rs.GetScreenActiveMode(screenId);
    if (mode.GetScreenWidth() > 0 && mode.GetScreenHeight() > 0) {
        cfg.screenId = screenId;
        cfg.width  = mode.GetScreenWidth();
        cfg.height = mode.GetScreenHeight();
        if (mode.GetScreenRefreshRate() > 0) {
            cfg.refreshMHz = mode.GetScreenRefreshRate() * 1000;
        }
    }
    HILOG_INFO(LOG_CORE, "screen %{public}llu: %{public}dx%{public}d @%{public}d mHz",
               static_cast<unsigned long long>(cfg.screenId),
               cfg.width, cfg.height, cfg.refreshMHz);
}

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    signal(SIGTERM, OnSignal);
    signal(SIGINT, OnSignal);
    signal(SIGPIPE, SIG_IGN);   /* a dying wayland client must not kill us */

    ServerConfig cfg;
    QueryDisplay(cfg);

    Server server;
    g_server = &server;
    if (!server.Init(cfg)) {
        HILOG_ERROR(LOG_CORE, "server init failed");
        return 1;
    }

    if (!server.Output().AttachSelfDrawingNode(cfg.screenId, cfg.width, cfg.height)) {
        HILOG_ERROR(LOG_CORE, "output attach failed");
        return 1;
    }

    /* Tell waydroidd the socket exists; it gates container start on this
     * (the hwc's own 5 s retry loop would absorb the race, but the
     * handshake keeps the logs clean — same shape as androidd's
     * android.composer.ready). */
    SetParameter("waydroid.compositor.ready", "1");

    HILOG_INFO(LOG_CORE, "entering event loop");
    server.Run();

    SetParameter("waydroid.compositor.ready", "0");
    HILOG_INFO(LOG_CORE, "exiting");
    return 0;
}
