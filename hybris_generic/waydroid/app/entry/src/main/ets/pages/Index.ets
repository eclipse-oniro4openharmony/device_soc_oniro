// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// Waydroid's page: the grid of Android apps, the first-run screens (download
// offer, progress, "turned off") and — behind the gear — the settings panel
// that manages the container itself (pages/ManagePanel).
//
// The grid is drawn from this app's own cache the moment the page appears
// (model/AppCache), whatever state the container is in, and refreshed once
// Android answers.  Opening this page is also what starts the stack on a
// device where nothing of it runs yet: the ability's SHOW beat does that, and
// by the time the user has found the app they want, Android is usually up.
// Unless it was stopped on purpose — then nothing is asked for until the user
// taps an app or presses Start.
//
// Tap: the app opens in a window of its own (AppWindowAbility, one per
// package).  Long press: end that app's Android task.

import type common from '@ohos.app.ability.common';
import hilog from '@ohos.hilog';
import promptAction from '@ohos.promptAction';
import type { BusinessError } from '@ohos.base';
import container, { STATE_READY } from '../model/Container';
import { AppCache, AppEntry } from '../model/AppCache';
import { Change, Control, KEY_ENABLED, KEY_WANT_RUN, RUN_OFF, RUN_ON } from '../model/Control';
import { poke, setQuiet, syncBeat } from '../model/Lifecycle';
import { ManagePanel } from './ManagePanel';
import {
  ImageSource, KEY_DETAIL, KEY_PHASE, KEY_PROGRESS, KEY_STATUS, KEY_SYS_URL, KEY_VEN_URL, Provisioner
} from '../model/Provisioner';

const DOMAIN = 0x2500;   // 16 bits for an app, or the line is dropped
const TAG = 'waydroid.grid';
const TICK_MS = 1000;
// Android's package list is not complete the moment it starts answering, and
// an APK can be installed inside the container while the grid is open: ask
// again every few ticks rather than once per visit.
const REFRESH_TICKS = 5;

@Entry
@Component
struct Index {
  @StorageProp(KEY_STATUS) @Watch('onStatus') status: string = '';
  @StorageProp(KEY_SYS_URL) sysUrl: string = '';
  @StorageProp(KEY_VEN_URL) venUrl: string = '';
  @StorageProp(KEY_PHASE) phase: string = '';
  @StorageProp(KEY_DETAIL) detail: string = '';
  @StorageProp(KEY_PROGRESS) progress: number = 0;
  @StorageProp(KEY_ENABLED) support: number = -1;   // not `enabled`: that is an ArkUI attribute
  @StorageProp(KEY_WANT_RUN) wantRun: string = RUN_ON;
  @State apps: AppEntry[] = [];
  @State ready: boolean = false;
  @State manage: boolean = false;

  private provisioner?: Provisioner;
  private control?: Control;
  private cache?: AppCache;
  private timer: number = -1;
  private refreshing: boolean = false;
  private fresh: boolean = false;       // refreshed since the page was last shown
  private ticks: number = 0;

  aboutToAppear(): void {
    const context = getContext(this) as common.UIAbilityContext;
    this.provisioner = new Provisioner(context);
    this.control = new Control(context.filesDir);
    this.cache = new AppCache(context.filesDir);
    this.apps = this.cache.load();
  }

  onPageShow(): void {
    this.fresh = false;                 // an APK installed meanwhile shows up now
    this.ticks = 0;
    if (this.timer < 0) {
      this.timer = setInterval(() => this.tick(), TICK_MS);
    }
    this.tick();
  }

  onPageHide(): void {
    if (this.timer >= 0) {
      clearInterval(this.timer);
      this.timer = -1;
    }
  }

  private tick(): void {
    this.ready = container.getState() === STATE_READY;
    if (!this.ready || this.refreshing || this.cache === undefined) {
      return;
    }
    this.ticks = this.ticks + 1;
    if (this.fresh && (this.ticks % REFRESH_TICKS) !== 0) {
      return;
    }
    this.refreshing = true;
    this.cache.refresh().then((apps: AppEntry[]) => {
      this.apps = apps;
      this.fresh = true;
      this.refreshing = false;
    }).catch((e: BusinessError) => {
      this.refreshing = false;          // frozen / still booting: next tick
      hilog.info(DOMAIN, TAG, 'refresh: %{public}s', JSON.stringify(e));
    });
  }

  // `bad-images`: nothing we hold is usable (a no-op while a download runs).
  onStatus(): void {
    if (this.status === 'bad-images') {
      this.provisioner?.discard();
    }
  }

  private working(): boolean {
    return this.phase === 'download' || this.phase === 'unpack';
  }

  private stopped(): boolean {
    return this.wantRun === RUN_OFF;
  }

  // After a finished download the supervisor goes on saying needs-images
  // until its next look (a few seconds); that is not a reason to offer the
  // button again.  bad-images always is.
  private wantsImages(): boolean {
    return this.status === 'bad-images' ||
      (this.status === 'needs-images' && this.phase !== 'done');
  }

  private startDownload(): void {
    if (this.provisioner === undefined || this.sysUrl === '' || this.venUrl === '') {
      return;
    }
    const src = new ImageSource();
    src.sysUrl = this.sysUrl;
    src.venUrl = this.venUrl;
    this.provisioner.run(src);
  }

  // Ask for the stack again: after Stop, or after the master switch was off.
  // The control file is what the supervisor obeys; the SHOW is what starts it
  // now rather than at the next beat.
  private resume(turnOn: boolean): void {
    const c = new Change();
    c.run = RUN_ON;
    if (turnOn) {
      c.enabled = 1;
      AppStorage.setOrCreate<number>(KEY_ENABLED, 1);
    }
    this.control?.apply(c);
    // The supervisor confirms within a beat or two; until then do not keep
    // showing the screen the user just left.
    AppStorage.setOrCreate<string>(KEY_STATUS, '');
    setQuiet(false);
    syncBeat();
    poke();
  }

  private open(app: AppEntry): void {
    if (this.stopped() || this.support === 0) {
      this.resume(this.support === 0);  // tapping an app is asking for Android
    }
    const context = getContext(this) as common.UIAbilityContext;
    context.startAbility({
      bundleName: context.abilityInfo.bundleName,
      abilityName: 'AppWindowAbility',
      parameters: { 'package': app.pkg, 'label': app.name }
    }).catch((e: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'startAbility(%{public}s): %{public}s', app.pkg, JSON.stringify(e));
    });
  }

  private close(app: AppEntry): void {
    container.closeApp(app.pkg);
    promptAction.showToast({ message: app.name + ' closed' });
  }

  @Builder
  Notice(title: string, body: string) {
    Text(title).fontSize(20).fontColor(0xFFFFFF).textAlign(TextAlign.Center).width('80%')
    if (body !== '') {
      Text(body).fontSize(14).fontColor(0xCCCCCC).textAlign(TextAlign.Center).width('80%').margin({ top: 12 })
    }
  }

  @Builder
  FirstRun() {
    Column() {
      if (this.working()) {
        this.Notice('Getting Android ready', '')
        Progress({ value: this.progress < 0 ? 0 : this.progress, total: 100, type: ProgressType.Linear })
          .width('80%').margin({ top: 28 }).color(0x3DDC84)
        Text(this.detail).fontSize(14).fontColor(0xCCCCCC).margin({ top: 12 })
        Text('Keep Waydroid open until this finishes.')
          .fontSize(12).fontColor(0x888888).margin({ top: 24 })
      } else if (this.status === 'disabled' || this.support === 0) {
        this.Notice('Android apps are turned off',
          'Nothing of the Android container runs on this device until you turn it back on.')
        Button('Turn on')
          .margin({ top: 28 }).backgroundColor(0x3DDC84).fontColor(0x000000)
          .onClick(() => this.resume(true))
      } else {
        this.Notice(this.status === 'bad-images' ? 'The Android image failed verification'
          : 'Android apps need a one-time download',
          'About 950 MB to download, 2 GB on the device. It is stored with this app: ' +
          'deleting it, or uninstalling Waydroid, frees the space again.')
        if (this.phase === 'error') {
          Text(this.detail).fontSize(13).fontColor(0xFF8A80).textAlign(TextAlign.Center)
            .width('80%').margin({ top: 12 })
        }
        Button(this.phase === 'error' || this.status === 'bad-images' ? 'Download again' : 'Download')
          .margin({ top: 28 }).backgroundColor(0x3DDC84).fontColor(0x000000)
          .onClick(() => this.startDownload())
      }
    }
    .justifyContent(FlexAlign.Center)
    .width('100%')
    .height('100%')
  }

  @Builder
  AppGrid() {
    Column() {
      Row() {
        Text('Waydroid').fontSize(26).fontWeight(FontWeight.Medium).fontColor(0xFFFFFF)
        Blank()
        if (this.stopped()) {
          Button('Start')
            .height(30).fontSize(13).backgroundColor(0x3DDC84).fontColor(0x000000)
            .onClick(() => this.resume(false))
        } else if (!this.ready) {
          LoadingProgress().width(22).height(22).color(0x3DDC84)
          Text(this.status === 'verifying' ? 'Checking…' : 'Starting Android…')
            .fontSize(13).fontColor(0xAAAAAA).margin({ left: 6 })
        }
        Image($r('app.media.ic_manage')).width(22).height(22)
          .opacity(0.7).margin({ left: 14 })
          .onClick(() => { this.manage = true; })
      }
      .width('100%').padding({ left: 24, right: 24, top: 20, bottom: 12 })

      if (this.apps.length === 0) {
        Column() {
          if (this.stopped()) {
            Text('Waydroid is stopped').fontSize(16).fontColor(0xCCCCCC)
            Text('Start it to see the apps installed in Android.')
              .fontSize(13).fontColor(0x8A9199).margin({ top: 8 })
          } else {
            LoadingProgress().width(56).height(56).color(0xFFFFFF)
            Text('Android is starting…').fontSize(16).fontColor(0xCCCCCC).margin({ top: 12 })
          }
        }
        .justifyContent(FlexAlign.Center).width('100%').layoutWeight(1)
      } else {
        Grid() {
          ForEach(this.apps, (app: AppEntry) => {
            GridItem() {
              Column() {
                if (app.iconUri !== '') {
                  Image(app.iconUri).width(60).height(60).borderRadius(14)
                } else {
                  Text(app.name.charAt(0)).fontSize(28).fontColor(0xFFFFFF)
                    .textAlign(TextAlign.Center).width(60).height(60)
                    .borderRadius(14).backgroundColor(0xFF2E3A40)
                }
                Text(app.name).fontSize(13).fontColor(0xE6E6E6).margin({ top: 8 })
                  .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
              }
              .width('100%').padding({ top: 12, bottom: 12 })
              .onClick(() => this.open(app))
              .gesture(LongPressGesture().onAction(() => this.close(app)))
            }
          }, (app: AppEntry) => app.pkg + app.iconUri)
        }
        .columnsTemplate('1fr 1fr 1fr 1fr')
        .rowsGap(8)
        .padding({ left: 12, right: 12 })
        .width('100%')
        .layoutWeight(1)
      }
    }
    .width('100%')
    .height('100%')
  }

  build() {
    Column() {
      if (this.manage) {
        ManagePanel({
          provisioner: this.provisioner,
          control: this.control,
          onClose: () => { this.manage = false; }
        })
      } else if (this.working() || this.status === 'disabled' || this.support === 0 ||
        this.wantsImages()) {
        this.FirstRun()
      } else {
        this.AppGrid()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(0xFF101418)
  }
}
