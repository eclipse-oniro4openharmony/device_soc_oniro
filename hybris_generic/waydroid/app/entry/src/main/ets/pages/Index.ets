// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// The grid of Android apps — and, before there is an Android to have apps,
// the first-run screens (download offer, progress, "turned off").
//
// The grid is drawn from this app's own cache the moment the page appears
// (model/AppCache), whatever state the container is in, and refreshed once
// Android answers.  Opening this page is also what starts the stack on a
// device where nothing of it runs yet: the ability's SHOW beat does that, and
// by the time the user has found the app they want, Android is usually up.
//
// Tap: the app opens in a window of its own (AppWindowAbility, one per
// package).  Long press: end that app's Android task.

import type common from '@ohos.app.ability.common';
import hilog from '@ohos.hilog';
import promptAction from '@ohos.promptAction';
import type { BusinessError } from '@ohos.base';
import container, { STATE_READY } from '../model/Container';
import { AppCache, AppEntry } from '../model/AppCache';
import {
  ImageSource, KEY_DETAIL, KEY_PHASE, KEY_PROGRESS, KEY_STATUS, KEY_SYS_URL, KEY_VEN_URL, Provisioner
} from '../model/Provisioner';

const DOMAIN = 0x2500;   // 16 bits for an app, or the line is dropped
const TAG = 'androidapps.grid';
const TICK_MS = 1000;

@Entry
@Component
struct Index {
  @StorageProp(KEY_STATUS) @Watch('onStatus') status: string = '';
  @StorageProp(KEY_SYS_URL) sysUrl: string = '';
  @StorageProp(KEY_VEN_URL) venUrl: string = '';
  @StorageProp(KEY_PHASE) phase: string = '';
  @StorageProp(KEY_DETAIL) detail: string = '';
  @StorageProp(KEY_PROGRESS) progress: number = 0;
  @State apps: AppEntry[] = [];
  @State ready: boolean = false;

  private provisioner?: Provisioner;
  private cache?: AppCache;
  private timer: number = -1;
  private refreshing: boolean = false;
  private fresh: boolean = false;       // refreshed since the page was last shown

  aboutToAppear(): void {
    const context = getContext(this) as common.UIAbilityContext;
    this.provisioner = new Provisioner(context);
    this.cache = new AppCache(context.filesDir);
    this.apps = this.cache.load();
  }

  onPageShow(): void {
    this.fresh = false;                 // an APK installed meanwhile shows up now
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
    if (!this.ready || this.fresh || this.refreshing || this.cache === undefined) {
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

  private open(app: AppEntry): void {
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
        Text('Keep Android Apps open until this finishes.')
          .fontSize(12).fontColor(0x888888).margin({ top: 24 })
      } else if (this.status === 'disabled') {
        this.Notice('Android apps are turned off on this device', '')
      } else {
        this.Notice(this.status === 'bad-images' ? 'The Android images failed verification'
          : 'Android apps need a one-time download',
          'About 950 MB to download, 2 GB on the device. It is stored with this app: ' +
          'uninstalling "Android Apps" frees the space again.')
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
        Text('Android apps').fontSize(26).fontWeight(FontWeight.Medium).fontColor(0xFFFFFF)
        Blank()
        if (!this.ready) {
          LoadingProgress().width(22).height(22).color(0x3DDC84)
          Text(this.status === 'verifying' ? 'Checking…' : 'Starting Android…')
            .fontSize(13).fontColor(0xAAAAAA).margin({ left: 6 })
        }
      }
      .width('100%').padding({ left: 24, right: 24, top: 20, bottom: 12 })

      if (this.apps.length === 0) {
        Column() {
          LoadingProgress().width(56).height(56).color(0xFFFFFF)
          Text('Android is starting…').fontSize(16).fontColor(0xCCCCCC).margin({ top: 12 })
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
      if (this.working() || this.status === 'disabled' || this.wantsImages()) {
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
