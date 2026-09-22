// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// "Android Apps": the launcher entry.  Its page is the grid of Android apps
// (pages/Index); each app then opens in a window of its own
// (appwindow/AppWindowAbility).
//
// Two conversations with the root side run from here, both as common events
// because an app can read nothing of the stack:
//   * out — the foreground lease (model/Lifecycle): SHOW beats while anything
//     of ours is in front.  The first beat is also what STARTS the stack on
//     demand (samgr, sa_profile/9601.json);
//   * in  — the supervisor's STATUS (disabled / needs-images / starting / …),
//     which the page turns into the first-run screens.

import UIAbility from '@ohos.app.ability.UIAbility';
import window from '@ohos.window';
import hilog from '@ohos.hilog';
import commonEventManager from '@ohos.commonEventManager';
import type { BusinessError } from '@ohos.base';
import { KEY_STATUS, KEY_SYS_URL, KEY_VEN_URL } from '../model/Provisioner';
import { enterForeground, leaveForeground, styleWindow } from '../model/Lifecycle';

// An app's hilog domain is 16 bits; anything above is silently dropped.
const DOMAIN = 0x2500;
const TAG = 'androidapps.ability';
// The compositor publishes this on the debug overlay's exit chord (Vol-Down +
// Vol-Up; see waydroid_compositor_main.cpp — not part of the product path).  Only
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

export default class EntryAbility extends UIAbility {
  private exitSubscriber?: commonEventManager.CommonEventSubscriber;
  private statusSubscriber?: commonEventManager.CommonEventSubscriber;

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

  onForeground(): void {
    enterForeground();
  }

  onBackground(): void {
    leaveForeground();
  }

  onDestroy(): void {
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
      styleWindow(windowStage);
    });
  }
}
