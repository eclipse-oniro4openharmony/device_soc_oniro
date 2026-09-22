// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// "Waydroid": the launcher entry.  Its page is the grid of Android apps plus
// the settings panel (pages/Index); each app then opens in a window of its own
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
import { enterForeground, leaveForeground, setQuiet, styleWindow } from '../model/Lifecycle';
import {
  Control, KEY_APPLIED_SEQ, KEY_AUTOSTART, KEY_ENABLED, KEY_IDLE, KEY_IMG_BYTES, KEY_IMG_LOC,
  KEY_SENT_SEQ, KEY_WANT_RUN, RUN_OFF
} from '../model/Control';

// An app's hilog domain is 16 bits; anything above is silently dropped.
const DOMAIN = 0x2500;
const TAG = 'waydroid.ability';
// The compositor publishes this on the debug overlay's exit chord (Vol-Down +
// Vol-Up; see waydroid_compositor_main.cpp — not part of the product path).  Only
// the app itself can reliably return OHOS to the home screen (StartAbility
// (home) does not background a foreground app under sceneboard, and
// MinimizeAllAppWindows is a silent no-op unless the launcher registered its
// callback), so the compositor asks us to background ourselves.
const EVENT_EXIT = 'org.oniroproject.waydroid.EXIT';
// The supervisor's side of the conversation: this app can read nothing of the
// stack, so it is told (see waydroid-supervisor.sh).  data is one state word —
//   disabled | stopped | verifying | starting | running |
//   needs-images | bad-images
// — followed by key=value tokens: what to download (sys= syssha= ven= vensha=),
// the settings as the root side really has them (en= as= idle=), which control
// seq it has applied (seq= run=) and where the images are (img= imgsz=).
const EVENT_STATUS = 'org.oniroproject.waydroid.STATUS';

export default class EntryAbility extends UIAbility {
  private exitSubscriber?: commonEventManager.CommonEventSubscriber;
  private statusSubscriber?: commonEventManager.CommonEventSubscriber;

  onCreate(): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
    AppStorage.setOrCreate<string>(KEY_STATUS, '');
    // What the user last asked for, before any of it is on screen: a device
    // stopped in a previous run must not be started by our first beat.
    new Control(this.context.filesDir).loadWanted();
    this.subscribeExit();
    this.subscribeStatus();
  }

  // AppStorage, not module variables: the in-tree ets build bundles the
  // ability and every page separately, so anything "static and shared" exists
  // once per bundle.  AppStorage is the one thing they really share.
  private onStatus(data: string): void {
    const f: string[] = data.trim().split(/\s+/);
    const state: string = f[0] ?? '';
    // A report older than our last request describes the world before it: the
    // `stopped` the supervisor published on its way out, arriving just after
    // the user pressed Start, would otherwise silence the beat that is
    // starting the stack.  The root side echoes seq= as soon as it applies.
    const sent: number = AppStorage.get<number>(KEY_SENT_SEQ) ?? 0;
    if (sent > 0 && this.seqOf(f) < sent) {
      return;
    }
    for (let i = 1; i < f.length; i++) {
      const t: string = f[i];
      const j: number = t.indexOf('=');
      if (j <= 0) {
        continue;
      }
      const k: string = t.substring(0, j);
      const v: string = t.substring(j + 1);
      if (k === 'sys') {
        AppStorage.setOrCreate<string>(KEY_SYS_URL, v);
      } else if (k === 'ven') {
        AppStorage.setOrCreate<string>(KEY_VEN_URL, v);
      } else if (k === 'seq') {
        AppStorage.setOrCreate<number>(KEY_APPLIED_SEQ, Number.parseInt(v, 10) || 0);
      } else if (k === 'en') {
        AppStorage.setOrCreate<number>(KEY_ENABLED, v === '1' ? 1 : 0);
      } else if (k === 'as') {
        AppStorage.setOrCreate<number>(KEY_AUTOSTART, v === '1' ? 1 : 0);
      } else if (k === 'idle') {
        AppStorage.setOrCreate<number>(KEY_IDLE, Number.parseInt(v, 10) || 0);
      } else if (k === 'img') {
        AppStorage.setOrCreate<string>(KEY_IMG_LOC, v);
      } else if (k === 'imgsz') {
        AppStorage.setOrCreate<number>(KEY_IMG_BYTES, Number.parseInt(v, 10) || 0);
      }
    }
    // `disabled` and `stopped` mean the stack is down on purpose: stop asking
    // for it until the user asks for it (model/Lifecycle).  `stopped` can also
    // come from a control file written before this run of the app.
    if (state === 'stopped') {
      AppStorage.setOrCreate<string>(KEY_WANT_RUN, RUN_OFF);
    }
    setQuiet(state === 'disabled' || state === 'stopped');
    if (AppStorage.get<string>(KEY_STATUS) !== state) {
      hilog.info(DOMAIN, TAG, 'status: %{public}s', state);
      AppStorage.setOrCreate<string>(KEY_STATUS, state);
    }
  }

  private seqOf(f: string[]): number {
    for (let i = 1; i < f.length; i++) {
      if (f[i].startsWith('seq=')) {
        return Number.parseInt(f[i].substring(4), 10) || 0;
      }
    }
    return 0;
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
