// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// "Manage Waydroid" — the settings panel behind the gear in the grid's header.
// Everything about the container a user can decide:
//
//   * the container itself — start it, stop it (its RAM back within seconds),
//     restart it when Android has got itself stuck;
//   * Android app support as a whole (persist.waydroid.enabled), whether the
//     container comes up at boot, and how long it may sit idle before it is
//     given up;
//   * the Android image — 2 GB that live in THIS app's storage: how big it is,
//     where it came from, download it again, or delete it and have the space
//     back without uninstalling the app.
//
// None of this is done here.  An app can not set a persist param, and the
// container runs as root: what the user asks for goes into the control file
// (model/Control), which the supervisor applies within two seconds and
// reports back through STATUS.  The one thing this panel reads for itself is
// the live container state, through the system NAPI module.
//
// It is a component, not a routed page: the in-tree ets build compiles a page
// per bundle, and a panel inside pages/Index shares the grid's Provisioner
// (so a download started here and one started on the first-run screen can not
// both run) and its AppStorage view of the world.

import hilog from '@ohos.hilog';
import promptAction from '@ohos.promptAction';
import container, { STATE_BOOTING, STATE_FROZEN, STATE_READY } from '../model/Container';
import {
  Change, Control, KEY_AUTOSTART, KEY_ENABLED, KEY_IDLE, KEY_IMG_BYTES, KEY_IMG_LOC,
  KEY_WANT_RUN, RUN_OFF, RUN_ON, RUN_RESTART
} from '../model/Control';
import { poke, setQuiet, syncBeat } from '../model/Lifecycle';
import {
  ImageSource, KEY_DETAIL, KEY_PHASE, KEY_PROGRESS, KEY_STATUS, KEY_SYS_URL, KEY_VEN_URL, Provisioner
} from '../model/Provisioner';

const DOMAIN = 0x2500;   // 16 bits for an app, or the line is dropped
const TAG = 'waydroid.manage';
const TICK_MS = 1000;
const GB = 1024 * 1024 * 1024;

class IdleChoice {
  label: string = '';
  seconds: number = 0;
}

const IDLE_CHOICES: IdleChoice[] = [
  { label: 'Never', seconds: 0 },
  { label: '5 min', seconds: 300 },
  { label: '30 min', seconds: 1800 },
  { label: '2 h', seconds: 7200 }
];

@Component
export struct ManagePanel {
  @StorageProp(KEY_STATUS) status: string = '';
  @StorageProp(KEY_ENABLED) support: number = -1;   // not `enabled`: that is an ArkUI attribute
  @StorageProp(KEY_AUTOSTART) autostart: number = -1;
  @StorageProp(KEY_IDLE) idle: number = -1;
  @StorageProp(KEY_IMG_LOC) imgLoc: string = '';
  @StorageProp(KEY_IMG_BYTES) imgBytes: number = 0;
  @StorageProp(KEY_WANT_RUN) wantRun: string = RUN_ON;
  @StorageProp(KEY_SYS_URL) sysUrl: string = '';
  @StorageProp(KEY_VEN_URL) venUrl: string = '';
  @StorageProp(KEY_PHASE) phase: string = '';
  @StorageProp(KEY_DETAIL) detail: string = '';
  @StorageProp(KEY_PROGRESS) progress: number = 0;
  @State state: number = -100;        // live, from the container
  @State ownBytes: number = 0;        // the copy of the images we could delete

  // Handed down by pages/Index, so that both screens drive the same download.
  provisioner: Provisioner | undefined = undefined;
  control: Control | undefined = undefined;
  onClose: () => void = () => {};

  private timer: number = -1;

  aboutToAppear(): void {
    this.tick();
    this.timer = setInterval(() => this.tick(), TICK_MS);
  }

  aboutToDisappear(): void {
    if (this.timer >= 0) {
      clearInterval(this.timer);
      this.timer = -1;
    }
  }

  private tick(): void {
    this.state = container.getState();
    this.ownBytes = this.provisioner?.imagesBytes() ?? 0;
  }

  private busy(): boolean {
    return this.phase === 'download' || this.phase === 'unpack';
  }

  private off(): boolean {
    return this.support === 0;
  }

  private stopped(): boolean {
    return this.wantRun === RUN_OFF;
  }

  private running(): boolean {
    return this.state === STATE_READY || this.state === STATE_FROZEN || this.state === STATE_BOOTING;
  }

  private ask(change: Change): void {
    if (this.control === undefined) {
      return;
    }
    this.control.apply(change);
    // The root side confirms within two seconds; until then show what was
    // asked for rather than what was.
    if (change.enabled >= 0) {
      AppStorage.setOrCreate<number>(KEY_ENABLED, change.enabled);
    }
    if (change.autostart >= 0) {
      AppStorage.setOrCreate<number>(KEY_AUTOSTART, change.autostart);
    }
    if (change.idleStop >= 0) {
      AppStorage.setOrCreate<number>(KEY_IDLE, change.idleStop);
    }
    if (change.run !== '' || change.enabled === 1) {
      // Anything but Stop is a request for the stack: let the beat through and
      // give samgr the SHOW that starts it now.
      if (change.run !== RUN_OFF) {
        setQuiet(false);
        poke();
      }
    }
    syncBeat();
  }

  private start(): void {
    const c = new Change();
    c.run = RUN_ON;
    this.ask(c);
    promptAction.showToast({ message: 'Starting Android…' });
  }

  private stop(): void {
    const c = new Change();
    c.run = RUN_OFF;
    this.ask(c);
    promptAction.showToast({ message: 'Stopping the container' });
  }

  private restart(): void {
    const c = new Change();
    c.run = RUN_RESTART;
    this.ask(c);
    promptAction.showToast({ message: 'Restarting the container' });
  }

  private setEnabled(on: boolean): void {
    const c = new Change();
    c.enabled = on ? 1 : 0;
    if (on) {
      c.run = RUN_ON;      // turning it back on is also a request to run
    }
    this.ask(c);
  }

  private download(): void {
    if (this.provisioner === undefined || this.sysUrl === '' || this.venUrl === '') {
      promptAction.showToast({ message: 'No download address yet — try again in a moment' });
      return;
    }
    const src = new ImageSource();
    src.sysUrl = this.sysUrl;
    src.venUrl = this.venUrl;
    this.provisioner.run(src);
  }

  private confirmDelete(): void {
    AlertDialog.show({
      title: 'Delete the Android image?',
      message: 'Android apps stop working until it is downloaded again (about 950 MB). ' +
        'The apps you installed inside Android are not touched.',
      primaryButton: {
        value: 'Cancel',
        action: () => {}
      },
      secondaryButton: {
        value: 'Delete',
        fontColor: 0xFF8A80,
        action: () => this.doDelete()
      }
    });
  }

  // No need to stop anything first: the supervisor looks at the images every
  // two seconds and tears the container down by itself when they go (the same
  // path an uninstall takes).  Asking for `run off` here would leave the
  // container stopped after the next download, which is not what was asked.
  private doDelete(): void {
    this.provisioner?.discard();
    this.ownBytes = 0;
    AppStorage.setOrCreate<number>(KEY_IMG_BYTES, 0);
    AppStorage.setOrCreate<string>(KEY_IMG_LOC, 'none');
    hilog.info(DOMAIN, TAG, 'images deleted by the user');
    promptAction.showToast({ message: 'Android image deleted' });
  }

  private stateTitle(): string {
    if (this.off()) {
      return 'Android app support is off';
    }
    if (this.status === 'needs-images' || (this.imgLoc === 'none' && this.ownBytes === 0)) {
      return 'No Android image yet';
    }
    if (this.status === 'bad-images') {
      return 'The Android image failed verification';
    }
    if (this.stopped()) {
      return 'Stopped';
    }
    if (this.state === STATE_READY) {
      return 'Running';
    }
    if (this.state === STATE_FROZEN) {
      return 'Running, frozen';
    }
    if (this.state === STATE_BOOTING || this.status === 'starting' || this.status === 'verifying') {
      return 'Starting…';
    }
    return 'Not running';
  }

  private stateDetail(): string {
    if (this.off()) {
      return 'Nothing of the container runs, and it will not start at boot.';
    }
    if (this.stopped()) {
      return 'The container is torn down; its memory is back. Start it, or just open an app.';
    }
    if (this.state === STATE_FROZEN) {
      return 'Nothing of it is on screen, so it is frozen and uses no CPU.';
    }
    if (this.state === STATE_READY) {
      return 'Android is up and answering.';
    }
    if (this.status === 'verifying') {
      return 'Checking the image against its checksums…';
    }
    if (this.running() || this.status === 'starting') {
      return 'Android is booting — about 20 seconds from cold.';
    }
    return 'It starts by itself when you open an app.';
  }

  private sizeText(bytes: number): string {
    return (bytes / GB).toFixed(2) + ' GB';
  }

  private imageTitle(): string {
    if (this.busy()) {
      return 'Getting the Android image…';
    }
    if (this.ownBytes > 0) {
      return this.sizeText(this.ownBytes) + ' — stored with this app';
    }
    if (this.imgLoc === 'data' && this.imgBytes > 0) {
      return this.sizeText(this.imgBytes) + ' — pushed from a host';
    }
    return 'Not downloaded';
  }

  private imageDetail(): string {
    if (this.ownBytes > 0) {
      return 'Deleting it here, or uninstalling Waydroid, gives the space back.';
    }
    if (this.imgLoc === 'data') {
      return 'In /data/waydroid/images. Only a shell can remove that copy.';
    }
    return 'About 950 MB to download, 2 GB on the device.';
  }

  @Builder
  CardTitle(text: string) {
    Text(text).fontSize(13).fontColor(0x8A9199).fontWeight(FontWeight.Medium)
      .width('100%').padding({ left: 4, bottom: 8, top: 20 })
  }

  @Builder
  SwitchRow(label: string, hint: string, on: boolean, which: number) {
    Row() {
      Column() {
        Text(label).fontSize(16).fontColor(0xFFFFFF).width('100%')
        Text(hint).fontSize(12).fontColor(0x8A9199).width('100%').margin({ top: 2 })
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      Toggle({ type: ToggleType.Switch, isOn: on })
        .selectedColor(0x3DDC84)
        .onChange((value: boolean) => {
          if (which === 0) {
            this.setEnabled(value);
          } else {
            const c = new Change();
            c.autostart = value ? 1 : 0;
            this.ask(c);
          }
        })
    }
    .width('100%').padding({ left: 16, right: 12, top: 14, bottom: 14 })
    .alignItems(VerticalAlign.Center)
  }

  build() {
    Column() {
      Row() {
        Text('Manage Waydroid').fontSize(22).fontWeight(FontWeight.Medium).fontColor(0xFFFFFF)
        Blank()
        Button('Done')
          .fontSize(14).fontColor(0x3DDC84).backgroundColor(0x00000000)
          .onClick(() => this.onClose())
      }
      .width('100%').padding({ left: 20, right: 12, top: 16, bottom: 4 })

      Scroll() {
        Column() {
          // ---------------------------------------------------- container
          this.CardTitle('CONTAINER')
          Column() {
            Row() {
              Text(this.stateTitle()).fontSize(17).fontColor(0xFFFFFF).layoutWeight(1)
              if (this.running() && !this.stopped() && this.state !== STATE_READY) {
                LoadingProgress().width(20).height(20).color(0x3DDC84)
              }
            }.width('100%')
            Text(this.stateDetail()).fontSize(12).fontColor(0x8A9199)
              .width('100%').margin({ top: 6 })
            Row() {
              if (this.stopped() || !this.running()) {
                Button('Start')
                  .backgroundColor(0x3DDC84).fontColor(0x000000).fontSize(14)
                  .enabled(!this.off())
                  .onClick(() => this.start())
              } else {
                Button('Stop')
                  .backgroundColor(0xFF2E3A40).fontColor(0xFFFFFF).fontSize(14)
                  .onClick(() => this.stop())
                Button('Restart')
                  .backgroundColor(0xFF2E3A40).fontColor(0xFFFFFF).fontSize(14)
                  .margin({ left: 10 })
                  .onClick(() => this.restart())
              }
            }.width('100%').margin({ top: 14 })
          }
          .width('100%').padding(16).borderRadius(14).backgroundColor(0xFF161C22)
          .alignItems(HorizontalAlign.Start)

          // ----------------------------------------------------- options
          this.CardTitle('OPTIONS')
          Column() {
            this.SwitchRow('Android app support', 'Off means nothing of Android ever runs',
              this.support === 1, 0)
            Divider().color(0xFF232B33)
            this.SwitchRow('Start at boot', 'Ready the moment you open an app, at the cost of its memory',
              this.autostart === 1, 1)
            Divider().color(0xFF232B33)
            Column() {
              Text('Stop when unused').fontSize(16).fontColor(0xFFFFFF).width('100%')
              Text('Give the container up after this long with nothing of it on screen')
                .fontSize(12).fontColor(0x8A9199).width('100%').margin({ top: 2 })
              Row() {
                ForEach(IDLE_CHOICES, (choice: IdleChoice) => {
                  Text(choice.label)
                    .fontSize(13)
                    .fontColor(this.idle === choice.seconds ? 0x000000 : 0xE6E6E6)
                    .backgroundColor(this.idle === choice.seconds ? 0x3DDC84 : 0xFF2E3A40)
                    .borderRadius(8).padding({ left: 12, right: 12, top: 6, bottom: 6 })
                    .margin({ right: 8 })
                    .onClick(() => {
                      const c = new Change();
                      c.idleStop = choice.seconds;
                      this.ask(c);
                    })
                }, (choice: IdleChoice) => choice.label)
              }.width('100%').margin({ top: 12 })
            }
            .width('100%').padding({ left: 16, right: 16, top: 14, bottom: 16 })
            .alignItems(HorizontalAlign.Start)
          }
          .width('100%').borderRadius(14).backgroundColor(0xFF161C22)

          // ------------------------------------------------------- image
          this.CardTitle('ANDROID IMAGE')
          Column() {
            Text(this.imageTitle()).fontSize(16).fontColor(0xFFFFFF).width('100%')
            Text(this.imageDetail()).fontSize(12).fontColor(0x8A9199)
              .width('100%').margin({ top: 4 })
            if (this.busy()) {
              Progress({ value: this.progress < 0 ? 0 : this.progress, total: 100,
                type: ProgressType.Linear })
                .width('100%').margin({ top: 14 }).color(0x3DDC84)
              Text(this.detail).fontSize(12).fontColor(0xCCCCCC).margin({ top: 8 })
            } else {
              Row() {
                Button(this.ownBytes > 0 ? 'Download again' : 'Download')
                  .backgroundColor(this.ownBytes > 0 ? 0xFF2E3A40 : 0x3DDC84)
                  .fontColor(this.ownBytes > 0 ? 0xFFFFFF : 0x000000).fontSize(14)
                  .onClick(() => this.download())
                if (this.ownBytes > 0) {
                  Button('Delete')
                    .backgroundColor(0xFF2E3A40).fontColor(0xFF8A80).fontSize(14)
                    .margin({ left: 10 })
                    .onClick(() => this.confirmDelete())
                }
              }.width('100%').margin({ top: 14 })
            }
          }
          .width('100%').padding(16).borderRadius(14).backgroundColor(0xFF161C22)
          .alignItems(HorizontalAlign.Start)

          Text('Android apps run in a Waydroid container. Stopping it closes every ' +
            'Android app; what they have saved is kept.')
            .fontSize(11).fontColor(0x6B7279).margin({ top: 20, bottom: 24 }).width('100%')
        }
        .width('100%').padding({ left: 16, right: 16 })
      }
      .layoutWeight(1).scrollBar(BarState.Off)
    }
    .width('100%').height('100%')
  }
}
