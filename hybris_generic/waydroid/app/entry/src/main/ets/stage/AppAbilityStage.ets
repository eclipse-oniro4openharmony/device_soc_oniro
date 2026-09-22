// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// AppWindowAbility is launchType "specified": one instance — one OHOS window,
// one recents card — per Android package.  The key is the package, so opening
// an app that already has a window brings that window back instead of
// stacking a second one on the same Android task.

import AbilityStage from '@ohos.app.ability.AbilityStage';
import type Want from '@ohos.app.ability.Want';

export default class AppAbilityStage extends AbilityStage {
  onAcceptWant(want: Want): string {
    if (want.abilityName === 'AppWindowAbility') {
      const pkg = want.parameters?.['package'];
      return typeof pkg === 'string' ? pkg : '';
    }
    return '';
  }
}
