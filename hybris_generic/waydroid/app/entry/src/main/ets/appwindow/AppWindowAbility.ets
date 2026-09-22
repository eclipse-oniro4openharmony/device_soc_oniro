// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// One Android app in one ordinary OHOS window.  The OHOS status bar, the
// gesture bar, the open/close animations and the recents card are the
// system's own, because this IS an ordinary window; what fills it is the
// Android task's frames, composited straight into the page's XComponent
// surface by waydroid_compositor (pages/AppWindow owns that).
//
// This ability owns the window's life:
//   * started with { package, label } by the grid (or by another window, when
//     Android itself opens a task of another package);
//   * the recents card carries the Android app's name;
//   * the compositor's TASK events decide when it ends —
//       inactive <mine>   while we are in front: the user backed out of the
//                         app's last activity (Android parked the task).
//                         Unless another task took over, in which case …
//       ontop <other>     … a task that has NO window yet (Android opened it
//                         itself: a picker, a share target) gets one, and
//                         this window just goes to the background, as on
//                         Android.  A task that already has a window is left
//                         alone: which of OUR windows is in front is the
//                         user's business, through OHOS.  (Reacting to those
//                         too made two open windows fight for the top for
//                         ever, each relaunching its app when it came to the
//                         front.)
//       gone <mine>       the task ended;
//   * swiping the card away ends the Android task (closeApp → the hwc removes
//     it), so nothing keeps running behind the user's back.

import UIAbility from '@ohos.app.ability.UIAbility';
import type AbilityConstant from '@ohos.app.ability.AbilityConstant';
import type Want from '@ohos.app.ability.Want';
import commonEventManager from '@ohos.commonEventManager';
import hilog from '@ohos.hilog';
import fs from '@ohos.file.fs';
import window from '@ohos.window';
import type { BusinessError } from '@ohos.base';
import container from '../model/Container';
import { AppCache } from '../model/AppCache';
import { enterForeground, leaveForeground, styleWindow } from '../model/Lifecycle';

const DOMAIN = 0x2500;   // 16 bits for an app, or the line is dropped
const TAG = 'androidapps.window';
const EVENT_TASK = 'org.oniroproject.waydroid.TASK';
// "inactive" and the "ontop" of whatever replaced us arrive a few ms apart.
const TAKEOVER_GRACE_MS = 500;
// Packages that have a window: "|pkg|pkg|".  AppStorage, because every
// ability is bundled separately and they all need the one list.
const KEY_WINDOWS = 'wdWindows';

function windows(): string {
  return AppStorage.get<string>(KEY_WINDOWS) ?? '|';
}

export default class AppWindowAbility extends UIAbility {
  private pkg: string = '';
  private label: string = '';
  private storage: LocalStorage = new LocalStorage();
  private subscriber?: commonEventManager.CommonEventSubscriber;
  private foreground: boolean = false;
  private takeoverAt: number = 0;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    const pkg = want.parameters?.['package'];
    const label = want.parameters?.['label'];
    this.pkg = typeof pkg === 'string' ? pkg : '';
    this.label = typeof label === 'string' && label !== '' ? label : this.pkg;
    hilog.info(DOMAIN, TAG, 'onCreate %{public}s', this.pkg);
    if (!windows().includes('|' + this.pkg + '|')) {
      AppStorage.setOrCreate<string>(KEY_WINDOWS, windows() + this.pkg + '|');
    }
    this.subscribeTasks();
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    const cache = new AppCache(this.context.filesDir);
    const iconPath = cache.iconPath(this.pkg);
    const hasIcon = fs.accessSync(iconPath);
    this.storage.setOrCreate<string>('package', this.pkg);
    this.storage.setOrCreate<string>('label', this.label);
    this.storage.setOrCreate<string>('iconUri', hasIcon ? 'file://' + iconPath : '');
    this.storage.setOrCreate<boolean>('onTop', false);
    this.storage.setOrCreate<boolean>('foreground', false);
    windowStage.loadContent('pages/AppWindow', this.storage, (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      styleWindow(windowStage);
    });
    // The recents card carries the Android app's name.  NOT its icon:
    // setMissionIcon works, but SceneBoard keeps one updated icon per BUNDLE
    // (UpdatedIcon/<bundle>.png), so every card of ours would show the icon
    // of whichever app was opened last.  A generic icon beats a wrong one;
    // per-app icons need per-app bundles (integration plan, phase A4).
    this.context.setMissionLabel(this.label).catch((e: BusinessError) => {
      hilog.warn(DOMAIN, TAG, 'setMissionLabel: %{public}s', JSON.stringify(e));
    });
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // Same package asked for again: we are simply brought to the front.
  }

  onForeground(): void {
    this.foreground = true;
    enterForeground();
    this.storage.setOrCreate<boolean>('foreground', true);
  }

  onBackground(): void {
    this.foreground = false;
    this.storage.setOrCreate<boolean>('foreground', false);
    leaveForeground();
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onDestroy %{public}s', this.pkg);
    AppStorage.setOrCreate<string>(KEY_WINDOWS, windows().replace('|' + this.pkg + '|', '|'));
    if (this.subscriber) {
      commonEventManager.unsubscribe(this.subscriber);
      this.subscriber = undefined;
    }
    // The card is gone (swiped away, or we ended ourselves): end the task.
    // Harmless when it already has.
    if (this.pkg !== '') {
      container.closeApp(this.pkg);
    }
  }

  private subscribeTasks(): void {
    const info: commonEventManager.CommonEventSubscribeInfo = { events: [EVENT_TASK] };
    commonEventManager.createSubscriber(info).then((subscriber) => {
      this.subscriber = subscriber;
      commonEventManager.subscribe(subscriber, (err, data) => {
        if (!err && data.event === EVENT_TASK) {
          this.onTask((data.data ?? '').trim().split(/\s+/));
        }
      });
    }).catch((e: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'createSubscriber(TASK) failed: %{public}s', JSON.stringify(e));
    });
  }

  private onTask(f: string[]): void {
    const what = f[0] ?? '';
    const pkg = f[1] ?? '';
    if (what === 'ontop' && pkg === this.pkg) {
      this.storage.setOrCreate<boolean>('onTop', true);
    } else if (what === 'ontop' && pkg !== '' && pkg !== '*' && this.foreground) {
      if (windows().includes('|' + pkg + '|')) {
        // If this really is Android falling back to that task because ours
        // was backed out of, "inactive <mine>" closes us and OHOS shows what
        // is next in ITS stack — whose window then puts its own app on top.
        return;
      }
      this.takeoverAt = Date.now();
      // Android opened a task of another package over us (a picker, a share
      // target): it gets its own window; we go to the background by that.
      hilog.info(DOMAIN, TAG, '%{public}s taken over by %{public}s', this.pkg, pkg);
      this.context.startAbility({
        bundleName: this.context.abilityInfo.bundleName,
        abilityName: 'AppWindowAbility',
        parameters: { 'package': pkg, 'label': this.labelOf(pkg) }
      }).catch((e: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'startAbility(%{public}s): %{public}s', pkg, JSON.stringify(e));
      });
    } else if (what === 'gone' && pkg === this.pkg) {
      hilog.info(DOMAIN, TAG, '%{public}s: task gone, closing', this.pkg);
      this.context.terminateSelf();
    } else if (what === 'inactive' && pkg === this.pkg && this.foreground) {
      setTimeout(() => {
        if (this.foreground && Date.now() - this.takeoverAt > TAKEOVER_GRACE_MS) {
          hilog.info(DOMAIN, TAG, '%{public}s: backed out of, closing', this.pkg);
          this.context.terminateSelf();
        }
      }, TAKEOVER_GRACE_MS);
    }
  }

  private labelOf(pkg: string): string {
    for (const app of new AppCache(this.context.filesDir).load()) {
      if (app.pkg === pkg) {
        return app.name;
      }
    }
    return pkg;
  }
}
