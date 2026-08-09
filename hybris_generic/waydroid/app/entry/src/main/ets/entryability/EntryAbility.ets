// Copyright (C) 2026 Oniro / Hybris Generic. Apache-2.0.
//
// Lifecycle bridge (W5): foreground thaws the container + shows the
// output; background freezes it (cgroup2 freezer) so a hidden Android
// session costs ~0 CPU.  The freeze/thaw is done natively via the
// compositor session SA (libandroidapps.so setForeground()).

import UIAbility from '@ohos.app.ability.UIAbility';
import window from '@ohos.window';
import hilog from '@ohos.hilog';
import nativeSession from 'libandroidapps.so';

const DOMAIN = 0xD002500;
const TAG = 'androidapps.ability';

export default class EntryAbility extends UIAbility {
  onCreate(): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      // Fullscreen so the container owns the whole panel.
      windowStage.getMainWindowSync().setWindowLayoutFullScreen(true);
    });
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, 'onForeground -> thaw container');
    nativeSession.setForeground(true);
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, 'onBackground -> freeze container');
    nativeSession.setForeground(false);
  }
}
