// Copyright (C) 2026 Oniro / Hybris Generic. Apache-2.0.
//
// What the user sees while there is no Android to look at.  The container's
// pixels are drawn by the compositor's self-drawing node, which sits above
// every window (Z=100000) — so once the stack runs this page is covered, and
// everything here is about the time before that: starting, or not being able
// to start.  The state comes from the supervisor's STATUS event (the ability
// subscribes); the one thing this page can DO is fetch the images.

import type common from '@ohos.app.ability.common';
import {
  ImageSource, KEY_DETAIL, KEY_PHASE, KEY_PROGRESS, KEY_STATUS, KEY_SYS_URL, KEY_VEN_URL, Provisioner
} from '../model/Provisioner';

@Entry
@Component
struct Index {
  @StorageProp(KEY_STATUS) @Watch('onStatus') status: string = '';
  @StorageProp(KEY_SYS_URL) sysUrl: string = '';
  @StorageProp(KEY_VEN_URL) venUrl: string = '';
  @StorageProp(KEY_PHASE) phase: string = '';
  @StorageProp(KEY_DETAIL) detail: string = '';
  @StorageProp(KEY_PROGRESS) progress: number = 0;

  private provisioner?: Provisioner;

  aboutToAppear(): void {
    this.provisioner = new Provisioner(getContext(this) as common.UIAbilityContext);
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

  @Builder
  Spinner(text: string) {
    LoadingProgress().width(64).height(64).color(0xFFFFFF)
    Text(text).fontSize(16).fontColor(0xCCCCCC).margin({ top: 12 })
  }

  build() {
    Column() {
      if (this.working()) {
        Text('Getting Android ready').fontSize(22).fontColor(0xFFFFFF)
        Progress({ value: this.progress < 0 ? 0 : this.progress, total: 100, type: ProgressType.Linear })
          .width('80%').margin({ top: 28 }).color(0x3DDC84)
        Text(this.detail).fontSize(14).fontColor(0xCCCCCC).margin({ top: 12 })
        Text('Keep Android Apps open until this finishes.')
          .fontSize(12).fontColor(0x888888).margin({ top: 24 })
      } else if (this.status === 'disabled') {
        Text('Android apps are turned off on this device')
          .fontSize(18).fontColor(0xFFFFFF).textAlign(TextAlign.Center).width('80%')
      } else if (this.wantsImages()) {
        Text(this.status === 'bad-images' ? 'The Android images failed verification'
          : 'Android apps need a one-time download')
          .fontSize(20).fontColor(0xFFFFFF).textAlign(TextAlign.Center).width('80%')
        Text('About 950 MB to download, 2 GB on the device. It is stored with this app: ' +
          'uninstalling "Android Apps" frees the space again.')
          .fontSize(14).fontColor(0xCCCCCC).textAlign(TextAlign.Center).width('80%').margin({ top: 12 })
        if (this.phase === 'error') {
          Text(this.detail).fontSize(13).fontColor(0xFF8A80).textAlign(TextAlign.Center)
            .width('80%').margin({ top: 12 })
        }
        Button(this.phase === 'error' || this.status === 'bad-images' ? 'Download again' : 'Download')
          .margin({ top: 28 }).backgroundColor(0x3DDC84).fontColor(0x000000)
          .onClick(() => this.startDownload())
      } else if (this.status === 'verifying' || this.status === 'needs-images') {
        this.Spinner('Checking the Android images…')
      } else {
        this.Spinner('Android is starting…')
      }
    }
    .justifyContent(FlexAlign.Center)
    .width('100%')
    .height('100%')
    .backgroundColor(0xFF000000)
  }
}
