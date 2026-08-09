// Copyright (C) 2026 Oniro / Hybris Generic. Apache-2.0.
//
// Thin ability shell (W5).  The container fore/background freeze/thaw is
// driven natively from the XComponent's surface lifecycle (see
// cpp/napi_init.cpp): the surface is created when this window shows and
// destroyed when it is torn down, so the native module thaws on
// create and freezes on destroy — no ArkTS<->native bridge needed.

import UIAbility from '@ohos.app.ability.UIAbility';
import window from '@ohos.window';
import hilog from '@ohos.hilog';

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
}
