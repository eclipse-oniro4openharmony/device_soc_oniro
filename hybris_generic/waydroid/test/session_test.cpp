/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * W5 bring-up tool: stand in for the Waydroid ArkUI front-end.
 *
 * The real app hands the compositor its fullscreen XComponent surface
 * over the session SA; here we do the same from a plain native process —
 * create our OWN RSSurfaceNode, hand its producer to the compositor, and
 * watch Android composite into a surface we own (not the compositor's
 * built-in self-drawing node).  That exercises the exact cross-process
 * producer-handoff + output-switch path the app uses, with no HAP build
 * in the loop.  Also drives InjectTouch and SetForeground(freeze).
 *
 *   session_test surface [seconds]   # hand over a producer surface
 *   session_test clear               # revert to the self-drawing node
 *   session_test touch <act> <id> <x> <y>
 *   session_test freeze | thaw
 *
 * v2 (launcher slice) — what the shell's NAPI module does, from a shell:
 *   session_test state
 *   session_test apps
 *   session_test icon <package> <file.png>
 *   session_test launch <package> | close <package>
 *   session_test window <package|-> [seconds] [height]
 *        a window of our own bound to <package> ("-" = whatever is on top),
 *        full width, <height> px tall from y=174 (under the OHOS status bar)
 *   session_test key <window> <evdev code>
 *   session_test asuid <uid> state     the caller gate: must print -2
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <grp.h>
#include <unistd.h>

#include <iremote_object.h>
#include <transaction/rs_interfaces.h>
#include <transaction/rs_transaction.h>
#include <ui/rs_surface_node.h>

#include "waydroid_session.h"

using namespace OHOS;
using namespace OHOS::Waydroid;

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s surface|clear|touch|freeze|thaw ...\n", argv[0]);
        return 2;
    }

    /* The caller gate: become an ordinary uid BEFORE the first IPC, so the
     * compositor sees that uid. */
    if (strcmp(argv[1], "asuid") == 0 && argc >= 4) {
        uid_t uid = static_cast<uid_t>(atoi(argv[2]));
        setgroups(0, nullptr);
        if (setgid(uid) != 0 || setuid(uid) != 0) {
            perror("setuid");
            return 1;
        }
        argv += 2;
        argc -= 2;
    }

    sptr<IWaydroidSession> session = WaydroidSessionProxy::Get();
    if (session == nullptr) {
        fprintf(stderr, "session SA not found — is the compositor running?\n");
        return 1;
    }

    if (strcmp(argv[1], "state") == 0) {
        printf("GetState -> %d (0 booting, 1 ready, 2 frozen, -2 denied)\n", session->GetState());
        return 0;
    }
    if (strcmp(argv[1], "apps") == 0) {
        std::vector<SessionApp> apps;
        int32_t rc = session->ListApps(apps);
        printf("ListApps -> %d, %zu apps\n", rc, apps.size());
        for (const SessionApp& app : apps) {
            printf("  %-16s %s\n", app.name.c_str(), app.package.c_str());
        }
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "icon") == 0 && argc >= 4) {
        std::vector<uint8_t> png;
        int32_t rc = session->GetAppIcon(argv[2], png);
        printf("GetAppIcon -> %d, %zu bytes\n", rc, png.size());
        if (rc == 0) {
            FILE* f = fopen(argv[3], "wb");
            if (f != nullptr) {
                fwrite(png.data(), 1, png.size(), f);
                fclose(f);
            }
        }
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "launch") == 0 && argc >= 3) {
        printf("LaunchApp -> %d\n", session->LaunchApp(argv[2]));
        return 0;
    }
    if (strcmp(argv[1], "close") == 0 && argc >= 3) {
        printf("CloseApp -> %d\n", session->CloseApp(argv[2]));
        return 0;
    }
    if (strcmp(argv[1], "key") == 0 && argc >= 4) {
        int32_t w = atoi(argv[2]);
        int32_t code = atoi(argv[3]);
        session->WindowKey(w, code, true);
        printf("WindowKey up -> %d\n", session->WindowKey(w, code, false));
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        printf("ClearOutputSurface -> %d\n", session->ClearOutputSurface());
        return 0;
    }
    if (strcmp(argv[1], "freeze") == 0) {
        printf("SetForeground(false) -> %d\n", session->SetForeground(false));
        return 0;
    }
    if (strcmp(argv[1], "thaw") == 0) {
        printf("SetForeground(true) -> %d\n", session->SetForeground(true));
        return 0;
    }
    if (strcmp(argv[1], "touch") == 0) {
        if (argc < 6) {
            fprintf(stderr, "touch <action> <id> <x> <y>\n");
            return 2;
        }
        int r = session->InjectTouch(atoi(argv[2]), atoi(argv[3]),
                                     atoi(argv[4]), atoi(argv[5]));
        printf("InjectTouch -> %d\n", r);
        return 0;
    }
    const bool asWindow = strcmp(argv[1], "window") == 0 && argc >= 3;
    if (strcmp(argv[1], "surface") != 0 && !asWindow) {
        fprintf(stderr, "unknown command %s\n", argv[1]);
        return 2;
    }

    int seconds = 30;
    if (asWindow) {
        seconds = (argc >= 4) ? atoi(argv[3]) : 30;
    } else if (argc >= 3) {
        seconds = atoi(argv[2]);
    }

    auto& rs = Rosen::RSInterfaces::GetInstance();
    Rosen::ScreenId screen = rs.GetDefaultScreenId();
    Rosen::RSScreenModeInfo mode = rs.GetScreenActiveMode(screen);
    int32_t w = mode.GetScreenWidth();
    int32_t h = mode.GetScreenHeight();
    if (w <= 0 || h <= 0) { w = 1080; h = 2400; }
    printf("display %llu: %dx%d\n", static_cast<unsigned long long>(screen), w, h);

    Rosen::RSSurfaceNodeConfig cfg;
    cfg.SurfaceNodeName = "AndroidAppsTestNode";
    cfg.isSync = false;
    auto node = Rosen::RSSurfaceNode::Create(
        cfg, Rosen::RSSurfaceNodeType::SELF_DRAWING_WINDOW_NODE, true, false);
    if (node == nullptr) {
        fprintf(stderr, "RSSurfaceNode::Create failed\n");
        return 1;
    }
    /* A window-shaped node: under the OHOS status bar, above its gesture
     * bar (174 / 84 px on the Plinius). */
    int32_t top = 0;
    if (asWindow) {
        top = 174;
        h = (argc >= 5) ? atoi(argv[4]) : h - 174 - 84;
    }
    node->SetPositionZ(100001.0f);   /* just above the compositor's own node */
    node->SetBounds({ 0, top, w, h });
    node->SetBackgroundColor(0xFF102030);
    node->SetFrameGravity(Rosen::Gravity::RESIZE);
    Rosen::RSTransaction::FlushImplicitTransaction();
    node->AttachToDisplay(screen);
    Rosen::RSTransaction::FlushImplicitTransaction();

    sptr<Surface> surface = node->GetSurface();
    if (surface == nullptr) {
        fprintf(stderr, "node has no surface\n");
        return 1;
    }
    sptr<IBufferProducer> producer = surface->GetProducer();
    if (producer == nullptr) {
        fprintf(stderr, "surface has no producer\n");
        return 1;
    }

    if (asWindow) {
        std::string package = strcmp(argv[2], "-") == 0 ? "" : argv[2];
        int32_t window = session->AttachWindow(package, producer->AsObject(), w, h);
        printf("AttachWindow('%s', %dx%d) -> window %d (holding %d s)\n", package.c_str(), w, h,
               window, seconds);
        if (window > 0) {
            session->SetWindowActive(window, true);
        }
        if (window > 0 && !package.empty()) {
            printf("LaunchApp -> %d\n", session->LaunchApp(package));
        }
        sleep(seconds);
        printf("DetachWindow -> %d\n", session->DetachWindow(window));
        return window > 0 ? 0 : 1;
    }

    int r = session->SetOutputSurface(producer->AsObject());
    printf("SetOutputSurface -> %d (holding node %d s; snapshot the panel now)\n",
           r, seconds);
    /* Keep the node alive: the compositor produces into it only while our
     * process (and thus the RS node) lives. */
    sleep(seconds);
    printf("done — reverting\n");
    session->ClearOutputSurface();
    return r == 0 ? 0 : 1;
}
