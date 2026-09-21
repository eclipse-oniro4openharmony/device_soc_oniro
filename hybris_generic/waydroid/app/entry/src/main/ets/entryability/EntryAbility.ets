// Copyright (C) 2026 Oniro / Hybris Generic. Apache-2.0.
//
// "Android Apps" launcher ability (W5, pure-ArkUI design).  This app owns
// no native code: it is only a launcher entry + lifecycle signal.  The
// container's pixels reach the panel through the compositor's self-drawing
// node (W3), and this ability publishes SHOW/HIDE common events on
// fore/background.  The compositor subscribes and
// WaydroidSessionStub::ApplyVisibility shows+thaws or hides+freezes the
// container, and moves the touch grab to match.  No inner samgr/surface
// libs in the HAP, so none of the app-sandbox native-load / namespace
// barriers apply (see README).
//
// SHOW is a heartbeat, not an edge: it is republished every SHOW_BEAT_MS for
// as long as the ability is in the foreground.  That one decision buys three
// things without any native code or extra permission:
//   * on-demand start — nothing of the stack runs until this app is opened;
//     samgr starts it from the first SHOW (sa_profile/9601.json), and a later
//     beat reveals the compositor that did not exist yet when the first fired;
//   * recovery — a container generation that died and was rebuilt comes up
//     hidden, and the next beat reveals it;
//   * safety — the compositor treats visibility as a lease, so if this app
//     dies without saying HIDE the panel and touch go back to OHOS.

import UIAbility from '@ohos.app.ability.UIAbility';
import window from '@ohos.window';
import hilog from '@ohos.hilog';
import commonEventManager from '@ohos.commonEventManager';
import type { BusinessError } from '@ohos.base';
import { KEY_STATUS, KEY_SYS_URL, KEY_VEN_URL } from '../model/Provisioner';

// An app's hilog domain is 16 bits; anything above is silently dropped.
const DOMAIN = 0x2500;
const TAG = 'androidapps.ability';
// Custom events the compositor subscribes to (see waydroid_compositor_main.cpp).
const EVENT_SHOW = 'org.oniroproject.waydroid.SHOW';
const EVENT_HIDE = 'org.oniroproject.waydroid.HIDE';
// The compositor publishes this on the exit chord (Vol-Down + Vol-Up).  Only
// the app itself can reliably return OHOS to the home screen (StartAbility
// (home) does not background a foreground app under sceneboard, and
// MinimizeAllAppWindows is a silent no-op unless the launcher registered its
// callback), so the compositor asks us to background ourselves.
const EVENT_EXIT = 'org.oniroproject.waydroid.EXIT';
// The supervisor's side of the conversation: this app can read nothing of the
// stack, so it is told (see waydroid-supervisor.sh).  data is one of
//   disabled | verifying | starting | running |
//   needs-images|bad-images <sys-url> <sys-sha256> <ven-url> <ven-sha256>
const EVENT_STATUS = 'org.oniroproject.waydroid.STATUS';
// Must stay well under the compositor's lease (kVisibilityLeaseMs, 10 s).
const SHOW_BEAT_MS = 2000;

function publishEvent(event: string, quiet: boolean = false): void {
  commonEventManager.publish(event, (err) => {
    if (err) {
      hilog.error(DOMAIN, TAG, 'publish %{public}s failed: %{public}s',
        event, JSON.stringify(err));
    } else if (!quiet) {
      hilog.info(DOMAIN, TAG, 'published %{public}s', event);
    }
  });
}

export default class EntryAbility extends UIAbility {
  private exitSubscriber?: commonEventManager.CommonEventSubscriber;
  private statusSubscriber?: commonEventManager.CommonEventSubscriber;
  private showBeat?: number;

  onCreate(): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
    AppStorage.setOrCreate<string>(KEY_STATUS, '');
    this.subscribeExit();
    this.subscribeStatus();
  }

  private onStatus(data: string): void {
    const f = data.trim().split(/\s+/);
    const state = f[0] ?? '';
    // AppStorage, not a module variable: the in-tree ets build bundles the
    // ability and every page separately, so a static shared "in code" exists
    // once per bundle.  AppStorage is the one thing they really share.
    if ((state === 'needs-images' || state === 'bad-images') && f.length >= 5) {
      AppStorage.setOrCreate<string>(KEY_SYS_URL, f[1]);
      AppStorage.setOrCreate<string>(KEY_VEN_URL, f[3]);
    }
    if (AppStorage.get<string>(KEY_STATUS) !== state) {
      hilog.info(DOMAIN, TAG, 'status: %{public}s', state);
      AppStorage.setOrCreate<string>(KEY_STATUS, state);
    }
  }

  private subscribeStatus(): void {
    const info: commonEventManager.CommonEventSubscribeInfo = { events: [EVENT_STATUS] };
    commonEventManager.createSubscriber(info).then((subscriber) => {
      this.statusSubscriber = subscriber;
      commonEventManager.subscribe(subscriber, (err, data) => {
        if (!err && data.event === EVENT_STATUS) {
          this.onStatus(data.data ?? '');
        }
      });
    }).catch((e: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'createSubscriber(STATUS) failed: %{public}s', JSON.stringify(e));
    });
  }

  // Subscribe to the compositor's EXIT broadcast (the Vol-Down+Vol-Up chord)
  // and, when it fires, send this ability to the background so OHOS shows its
  // home screen.  moveAbilityToBackground only works from the foreground —
  // which is exactly when the container (and so the chord) is in use.  Going
  // to background also fires onBackground → HIDE, hiding+freezing the
  // container, so the whole exit flows through one path.
  private subscribeExit(): void {
    const info: commonEventManager.CommonEventSubscribeInfo = { events: [EVENT_EXIT] };
    commonEventManager.createSubscriber(info).then((subscriber) => {
      this.exitSubscriber = subscriber;
      commonEventManager.subscribe(subscriber, (err, data) => {
        if (err) {
          hilog.error(DOMAIN, TAG, 'exit subscribe cb err: %{public}s', JSON.stringify(err));
          return;
        }
        if (data.event === EVENT_EXIT) {
          hilog.info(DOMAIN, TAG, 'EXIT chord received; backgrounding to home');
          this.context.moveAbilityToBackground().then(() => {
            hilog.info(DOMAIN, TAG, 'moved to background');
          }).catch((e: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'moveAbilityToBackground failed: %{public}s',
              JSON.stringify(e));
          });
        }
      });
    }).catch((e: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'createSubscriber(EXIT) failed: %{public}s', JSON.stringify(e));
    });
  }

  private stopShowBeat(): void {
    if (this.showBeat !== undefined) {
      clearInterval(this.showBeat);
      this.showBeat = undefined;
    }
  }

  onForeground(): void {
    // App in the foreground → start the stack if it is down, show + thaw the
    // container, and keep saying so (see the header).
    publishEvent(EVENT_SHOW);
    this.stopShowBeat();
    this.showBeat = setInterval(() => publishEvent(EVENT_SHOW, true), SHOW_BEAT_MS);
  }

  onBackground(): void {
    // App left the foreground → hide + freeze the container so it costs no
    // CPU and OHOS gets the panel and touch back.
    this.stopShowBeat();
    publishEvent(EVENT_HIDE);
  }

  onDestroy(): void {
    this.stopShowBeat();
    publishEvent(EVENT_HIDE);
    if (this.exitSubscriber) {
      commonEventManager.unsubscribe(this.exitSubscriber);
      this.exitSubscriber = undefined;
    }
    if (this.statusSubscriber) {
      commonEventManager.unsubscribe(this.statusSubscriber);
      this.statusSubscriber = undefined;
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      // Fullscreen so the container overlay owns the whole panel.
      windowStage.getMainWindowSync().setWindowLayoutFullScreen(true);
    });
  }
}
