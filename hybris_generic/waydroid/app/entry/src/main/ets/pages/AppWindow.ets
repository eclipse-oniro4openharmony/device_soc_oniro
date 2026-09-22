// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// The inside of an Android app's OHOS window: one XComponent, handed to the
// compositor, plus a splash until that app's own first frame is on it.
//
// The page drives a small state machine off one timer instead of reacting to
// events, because every precondition can come and go under it: the stack may
// not exist yet (on-demand start: our SHOW beat is what starts it), Android
// may still be booting, the compositor may have been rebuilt by the
// supervisor (a new generation forgets its windows).  Each tick just makes
// true what should be: a live window for our surface, our app launched into
// it.
//
//   surface ready ─┐
//   Android ready ─┴─► attachWindow(surface, package, w, h) ─► launchApp(package)
//                          ▲                                        │
//                          └── windowAlive() false (new generation) ┘
//
// The window's size is what Android's display becomes (the compositor
// configures the hwc with it), so Android lays out for exactly the area
// between the OHOS status bar and the OHOS gesture bar.

import container, { KEY_BACK, STATE_FROZEN, STATE_READY } from '../model/Container';

const TICK_MS = 400;
// A launch can be swallowed: this hwc lets Android's display sleep while no
// window of it is engaged, and the launch that finds it asleep only wakes it
// (PowerManager: "Waking up ... details=waydroid:launchApp") — no window, no
// frame.  Launching again is harmless, so: no frame of ours yet, ask again.
const RELAUNCH_MS = 3000;
// OHOS PointerEvent action codes, which the compositor speaks.
const ACTION_CANCEL = 1;
const ACTION_DOWN = 2;
const ACTION_MOVE = 3;
const ACTION_UP = 4;

@Entry({ useSharedStorage: true })
@Component
struct AppWindow {
  @LocalStorageProp('package') pkg: string = '';
  @LocalStorageProp('label') label: string = '';
  @LocalStorageProp('iconUri') iconUri: string = '';
  @LocalStorageLink('onTop') onTop: boolean = false;
  @LocalStorageProp('foreground') @Watch('onForegroundChanged') foreground: boolean = false;
  @State hint: string = '';

  private xc: XComponentController = new XComponentController();
  private surfaceId: string = '';
  private widthPx: number = 0;
  private heightPx: number = 0;
  private windowId: number = -1;
  private launched: boolean = false;
  private launchedAt: number = 0;
  private timer: number = -1;
  private waited: number = 0;

  aboutToAppear(): void {
    this.timer = setInterval(() => this.tick(), TICK_MS);
  }

  aboutToDisappear(): void {
    if (this.timer >= 0) {
      clearInterval(this.timer);
      this.timer = -1;
    }
    if (this.windowId > 0) {
      container.detachWindow(this.windowId);
      this.windowId = -1;
    }
  }

  // Coming back to the front: Android may be showing another task by now
  // (another of our windows was used meanwhile).  Launching again is how a
  // running task is brought to the top.
  onForegroundChanged(): void {
    if (this.windowId > 0) {
      // Which of our windows is in front is what the compositor activates —
      // and the hwc makes the activated window's task Android's focused one.
      container.setWindowActive(this.windowId, this.foreground);
    }
    if (this.foreground) {
      this.launched = false;
      this.tick();
    }
  }

  onBackPress(): boolean {
    if (this.windowId > 0 && this.onTop) {
      container.sendKey(this.windowId, KEY_BACK);   // the OHOS back gesture is Android's BACK
      return true;
    }
    return false;                                   // still on the splash: just close
  }

  private tick(): void {
    if (!this.foreground || this.pkg === '') {
      return;
    }
    const state = container.getState();
    if (state !== STATE_READY) {
      // Frozen is momentary (our own SHOW beat thaws it); the rest is a cold
      // start, ~15 s the first time after a boot or an idle stop.
      this.waited += TICK_MS;
      this.hint = (state === STATE_FROZEN || this.waited < 1500) ? '' : 'Starting Android…';
      return;
    }
    this.waited = 0;
    if (this.surfaceId === '' || this.widthPx <= 0 || this.heightPx <= 0) {
      return;
    }
    if (this.windowId <= 0 || !container.windowAlive(this.windowId)) {
      this.onTop = false;
      this.launched = false;
      this.windowId = container.attachWindow(this.surfaceId, this.pkg, this.widthPx, this.heightPx);
      if (this.windowId <= 0) {
        return;
      }
      container.setWindowActive(this.windowId, true);
    }
    if (this.launched && !this.onTop && Date.now() - this.launchedAt > RELAUNCH_MS) {
      this.launched = false;
    }
    if (!this.launched) {
      this.launched = true;
      this.launchedAt = Date.now();
      this.hint = '';
      container.launchApp(this.pkg).then((rc: number) => {
        if (rc !== 0) {
          this.launched = false;        // frozen or not ready after all: next tick
        }
      });
    }
  }

  private forward(event: TouchEvent): void {
    if (this.windowId <= 0) {
      return;
    }
    let action = ACTION_MOVE;
    if (event.type === TouchType.Down) {
      action = ACTION_DOWN;
    } else if (event.type === TouchType.Up) {
      action = ACTION_UP;
    } else if (event.type === TouchType.Cancel) {
      action = ACTION_CANCEL;
    }
    for (const t of event.changedTouches) {
      container.sendTouch(this.windowId, action, t.id, Math.round(vp2px(t.x)), Math.round(vp2px(t.y)));
    }
  }

  build() {
    Stack() {
      // One pixel taller than the window and shifted up by it: Android keeps a
      // 1 px status bar (0 would break touch, see ../../../../../data/overlay)
      // and that row, with its clipped icon tops, would show as a hairline.
      // This puts it just outside the clip.  Touch is relative to the
      // component, so coordinates stay right.
      XComponent({ type: XComponentType.SURFACE, controller: this.xc })
        .width('100%')
        .height('calc(100% + 1px)')
        .position({ x: 0, y: '-1px' })
        .onLoad(() => {
          this.surfaceId = this.xc.getXComponentSurfaceId();
          this.tick();
        })
        .onSizeChange((before: SizeOptions, after: SizeOptions) => {
          this.widthPx = Math.round(vp2px(after.width as number));
          this.heightPx = Math.round(vp2px(after.height as number));
        })
        .onTouch((event: TouchEvent) => this.forward(event))

      if (!this.onTop) {
        Column() {
          if (this.iconUri !== '') {
            Image(this.iconUri).width(96).height(96).borderRadius(22)
          }
          Text(this.label).fontSize(20).fontColor(0xFFFFFF).margin({ top: 20 })
          if (this.hint !== '') {
            LoadingProgress().width(36).height(36).color(0xCCCCCC).margin({ top: 36 })
            Text(this.hint).fontSize(14).fontColor(0xAAAAAA).margin({ top: 8 })
          }
        }
        .justifyContent(FlexAlign.Center)
        .width('100%')
        .height('100%')
        .backgroundColor(0xFF101418)
      }
    }
    .width('100%')
    .height('100%')
    .clip(true)
    .backgroundColor(0xFF101418)
  }
}
