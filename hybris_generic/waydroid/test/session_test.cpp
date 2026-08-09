/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * W5 bring-up tool: stand in for the "Android Apps" ArkUI front-end.
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
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    sptr<IWaydroidSession> session = WaydroidSessionProxy::Get();
    if (session == nullptr) {
        fprintf(stderr, "session SA not found — is the compositor running?\n");
        return 1;
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
    if (strcmp(argv[1], "surface") != 0) {
        fprintf(stderr, "unknown command %s\n", argv[1]);
        return 2;
    }

    int seconds = (argc >= 3) ? atoi(argv[2]) : 30;

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
    node->SetPositionZ(100001.0f);   /* just above the compositor's own node */
    node->SetBounds({ 0, 0, w, h });
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
