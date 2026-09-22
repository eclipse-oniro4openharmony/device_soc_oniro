// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// "Something of ours is in the foreground" — the lease the compositor thaws
// the container on (SHOW, republished every SHOW_BEAT_MS) and freezes it
// without (HIDE, or five missed beats).  See the README for what the
// heartbeat buys: on-demand start, recovery, crash safety.
//
// Several abilities share it (the grid, one window per Android app), so it is
// counted: SHOW while the count is positive, HIDE when it has stayed at zero
// for a moment — moving from the grid into an app window is one ability going
// to the background and another coming to the foreground, in either order,
// and must not freeze Android in between.
//
// The state lives in AppStorage, not in module variables: the in-tree ets
// build bundles every ability and page separately, so a module variable
// exists once per bundle.

import commonEventManager from '@ohos.commonEventManager';
import hilog from '@ohos.hilog';
import window from '@ohos.window';
import type { BusinessError } from '@ohos.base';
import { KEY_WANT_RUN, RUN_OFF } from './Control';

const DOMAIN = 0x2500;   // 16 bits for an app, or the line is dropped
const TAG = 'waydroid.lease';
const EVENT_SHOW = 'org.oniroproject.waydroid.SHOW';
const EVENT_HIDE = 'org.oniroproject.waydroid.HIDE';
// Must stay well under the compositor's lease (kVisibilityLeaseMs, 10 s).
const SHOW_BEAT_MS = 2000;
const HIDE_GRACE_MS = 400;
const KEY_COUNT = 'wdForegroundCount';
const KEY_BEAT = 'wdBeatTimer';
// The beat is also what STARTS the stack on demand, so it has to stop when
// the stack is meant to be down: the user pressed Stop (KEY_WANT_RUN), or the
// supervisor answered `disabled`.  Without this, being in the foreground with
// Android turned off would start a supervisor every couple of seconds, each
// of which tears itself down again.
const KEY_QUIET = 'wdBeatQuiet';

function publish(event: string): void {
  commonEventManager.publish(event, (err) => {
    if (err) {
      hilog.error(DOMAIN, TAG, 'publish %{public}s failed: %{public}s', event, JSON.stringify(err));
    }
  });
}

function count(): number {
  return AppStorage.get<number>(KEY_COUNT) ?? 0;
}

function wanted(): boolean {
  return (AppStorage.get<string>(KEY_WANT_RUN) ?? '') !== RUN_OFF &&
    !(AppStorage.get<boolean>(KEY_QUIET) ?? false);
}

function startBeat(): void {
  publish(EVENT_SHOW);
  if ((AppStorage.get<number>(KEY_BEAT) ?? -1) < 0) {
    AppStorage.setOrCreate<number>(KEY_BEAT, setInterval(() => publish(EVENT_SHOW), SHOW_BEAT_MS));
  }
}

function stopBeat(): void {
  const timer = AppStorage.get<number>(KEY_BEAT) ?? -1;
  if (timer >= 0) {
    clearInterval(timer);
    AppStorage.setOrCreate<number>(KEY_BEAT, -1);
  }
}

export function enterForeground(): void {
  AppStorage.setOrCreate<number>(KEY_COUNT, count() + 1);
  if (wanted()) {
    startBeat();
  }
}

// The stack is down on purpose (Stop, or `disabled`): stop asking for it.  A
// beat resumes on the next request — Start, Turn on, or opening an app.
export function setQuiet(quiet: boolean): void {
  if ((AppStorage.get<boolean>(KEY_QUIET) ?? false) === quiet) {
    return;
  }
  AppStorage.setOrCreate<boolean>(KEY_QUIET, quiet);
  hilog.info(DOMAIN, TAG, quiet ? 'the stack is meant to be down: no beat' : 'beating again');
  syncBeat();
}

// Make the beat match what is wanted right now.  Called after every change of
// KEY_WANT_RUN (the Start/Stop buttons) or of KEY_QUIET.
export function syncBeat(): void {
  if (count() > 0 && wanted()) {
    startBeat();
  } else {
    stopBeat();
  }
}

// One SHOW, outside the beat: what makes samgr start the stack the moment the
// user asks for it, instead of up to two seconds later.
export function poke(): void {
  publish(EVENT_SHOW);
}

export function leaveForeground(): void {
  AppStorage.setOrCreate<number>(KEY_COUNT, Math.max(0, count() - 1));
  setTimeout(() => {
    if (count() === 0) {
      stopBeat();
      publish(EVENT_HIDE);
      hilog.info(DOMAIN, TAG, 'nothing of ours in the foreground: HIDE');
    }
  }, HIDE_GRACE_MS);
}

// The strips under the OHOS status bar and gesture bar are the window's own
// background: make them the app's colour, with light status-bar content.
export function styleWindow(windowStage: window.WindowStage): void {
  try {
    const win = windowStage.getMainWindowSync();
    win.setWindowBackgroundColor('#101418');
    win.setWindowSystemBarProperties({
      statusBarColor: '#101418',
      statusBarContentColor: '#FFFFFF',
      navigationBarColor: '#101418',
      navigationBarContentColor: '#FFFFFF'
    }).catch((e: BusinessError) => {
      hilog.warn(DOMAIN, TAG, 'setWindowSystemBarProperties: %{public}s', JSON.stringify(e));
    });
  } catch (e) {
    hilog.warn(DOMAIN, TAG, 'styleWindow: %{public}s', JSON.stringify(e));
  }
}
