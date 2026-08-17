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

import UIAbility from '@ohos.app.ability.UIAbility';
import window from '@ohos.window';
import hilog from '@ohos.hilog';
import commonEventManager from '@ohos.commonEventManager';
import type { BusinessError } from '@ohos.base';

const DOMAIN = 0xD002500;
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

function publishEvent(event: string): void {
  commonEventManager.publish(event, (err) => {
    if (err) {
      hilog.error(DOMAIN, TAG, 'publish %{public}s failed: %{public}s',
        event, JSON.stringify(err));
    } else {
      hilog.info(DOMAIN, TAG, 'published %{public}s', event);
    }
  });
}

export default class EntryAbility extends UIAbility {
  private exitSubscriber?: commonEventManager.CommonEventSubscriber;

  onCreate(): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
    this.subscribeExit();
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
    // App in the foreground → show + thaw the container.
    publishEvent(EVENT_SHOW);
  }

  onBackground(): void {
    // App left the foreground → hide + freeze the container so it costs no
    // CPU and OHOS gets the panel and touch back.
    publishEvent(EVENT_HIDE);
  }

  onDestroy(): void {
    publishEvent(EVENT_HIDE);
    if (this.exitSubscriber) {
      commonEventManager.unsubscribe(this.exitSubscriber);
      this.exitSubscriber = undefined;
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
