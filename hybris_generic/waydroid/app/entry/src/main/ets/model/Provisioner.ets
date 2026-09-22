// Copyright (C) 2026 Oniro / Hybris Generic. Apache-2.0.
//
// Provisioning: fetch the two upstream Waydroid images into THIS app's
// storage.  Living here rather than in a root-owned directory is the point —
// deleting them from the settings panel, or uninstalling Waydroid, gives the
// 2 GB back.
//
// This app is not trusted with them.  What and where to download comes from
// the supervisor's STATUS event (it reads images.manifest, which ships in the
// system image); and before anything is mounted, waydroidd checks the files
// against the sha256 pins in that same manifest, takes ownership of them and
// makes sure nobody still holds them open for writing.  So nothing is
// verified here: a bad download is reported back as `bad-images`.
//
// The images appear atomically: everything happens in images.tmp/, which is
// renamed to images/ at the very end, so the supervisor never sees half a file.

import fs from '@ohos.file.fs';
import hilog from '@ohos.hilog';
import request from '@ohos.request';
import statvfs from '@ohos.file.statvfs';
import zlib from '@ohos.zlib';
import type common from '@ohos.app.ability.common';
import type { BusinessError } from '@ohos.base';

const DOMAIN = 0x2500;   // 16 bits for an app, or the line is dropped
const TAG = 'waydroid.provision';

// 0.95 GB of zips + 2.07 GB unpacked; the system zip is deleted before the
// vendor one is fetched, but leave headroom for Android's own data.img.
const NEEDED_BYTES = 3.5 * 1024 * 1024 * 1024;

// AppStorage keys.  The ability writes the first three from the supervisor's
// STATUS event; the page (which owns the Provisioner) writes the rest.
export const KEY_STATUS = 'wdStatus';
export const KEY_SYS_URL = 'wdSysUrl';
export const KEY_VEN_URL = 'wdVenUrl';
export const KEY_PHASE = 'wdPhase';        // '' | 'download' | 'unpack' | 'done' | 'error'
export const KEY_DETAIL = 'wdDetail';      // human-readable line
export const KEY_PROGRESS = 'wdProgress';  // 0..100, -1 = indeterminate

export class ImageSource {
  sysUrl: string = '';
  venUrl: string = '';
}

function report(phase: string, detail: string, progress: number): void {
  AppStorage.setOrCreate<string>(KEY_PHASE, phase);
  AppStorage.setOrCreate<string>(KEY_DETAIL, detail);
  AppStorage.setOrCreate<number>(KEY_PROGRESS, progress);
}

function mb(n: number): string {
  return (n / (1024 * 1024)).toFixed(0);
}

export class Provisioner {
  private ctx: common.UIAbilityContext;
  private busy: boolean = false;

  constructor(ctx: common.UIAbilityContext) {
    this.ctx = ctx;
  }

  imagesDir(): string {
    return this.ctx.filesDir + '/images';
  }

  private tmpDir(): string {
    return this.ctx.filesDir + '/images.tmp';
  }

  isBusy(): boolean {
    return this.busy;
  }

  // What our own copy of the images costs, for the settings panel.  0 when
  // there is none (a host-pushed copy in /data/waydroid is not ours to weigh
  // or to delete — the supervisor reports that one instead).
  imagesBytes(): number {
    let total: number = 0;
    const names: string[] = ['system.img', 'vendor.img'];
    for (const name of names) {
      try {
        total += fs.statSync(this.imagesDir() + '/' + name).size;
      } catch (e) {
        // not there: nothing to add
      }
    }
    return total;
  }

  // The supervisor said `bad-images`: whatever we have is useless.
  discard(): void {
    if (this.busy) {
      return;
    }
    this.removeTree(this.imagesDir());
    this.removeTree(this.tmpDir());
  }

  private removeTree(dir: string): void {
    try {
      if (fs.accessSync(dir)) {
        fs.rmdirSync(dir);   // recursive on OpenHarmony
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'rm %{public}s: %{public}s', dir, JSON.stringify(e));
    }
  }

  private download(url: string, filePath: string, label: string, base: number,
    span: number): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const config: request.DownloadConfig = {
        url: url,
        filePath: filePath,
        enableMetered: true,
        enableRoaming: false,
        background: true,
        title: 'Waydroid — ' + label
      };
      // The download service queues a task silently while the device is
      // offline (and WiFi has a way of coming up off on this phone): say so
      // instead of sitting at 0 %.
      let started = false;
      const hint = setTimeout(() => {
        if (!started) {
          report('download', 'Waiting for a network connection…', base);
        }
      }, 8000);
      request.downloadFile(this.ctx, config).then((task: request.DownloadTask) => {
        task.on('progress', (received: number, total: number) => {
          started = true;
          const frac = total > 0 ? received / total : 0;
          const size = total > 0 ? mb(received) + ' / ' + mb(total) + ' MB' : mb(received) + ' MB';
          report('download', 'Downloading ' + label + ' — ' + size, base + frac * span);
        });
        task.on('complete', () => {
          clearTimeout(hint);
          resolve();
        });
        task.on('fail', (err: number) => {
          clearTimeout(hint);
          reject(new Error('download of ' + label + ' failed (code ' + err + ')'));
        });
      }).catch((e: BusinessError) => {
        clearTimeout(hint);
        reject(new Error('can not start the download of ' + label + ': ' + e.message));
      });
    });
  }

  private async fetchOne(url: string, label: string, img: string, base: number): Promise<void> {
    const tmp = this.tmpDir();
    const zip = tmp + '/' + img + '.zip';
    // Downloading is ~70 % of one image's share of the bar, unpacking the rest.
    await this.download(url, zip, label, base, 35);
    report('unpack', 'Unpacking ' + label + '…', base + 35);
    await zlib.decompressFile(zip, tmp);
    fs.unlinkSync(zip);
    if (!fs.accessSync(tmp + '/' + img)) {
      throw new Error(label + ': the archive did not contain ' + img);
    }
  }

  async run(src: ImageSource): Promise<void> {
    const phase: string = AppStorage.get<string>(KEY_PHASE) ?? '';
    if (this.busy || phase === 'download' || phase === 'unpack') {
      return;
    }
    this.busy = true;
    try {
      const free = statvfs.getFreeSizeSync(this.ctx.filesDir);
      if (free < NEEDED_BYTES) {
        throw new Error('not enough free storage: ' + mb(free) + ' MB free, about ' +
          mb(NEEDED_BYTES) + ' MB needed');
      }
      this.removeTree(this.tmpDir());
      this.removeTree(this.imagesDir());
      fs.mkdirSync(this.tmpDir());
      report('download', 'Starting the download…', 0);
      await this.fetchOne(src.sysUrl, 'Android system image', 'system.img', 0);
      await this.fetchOne(src.venUrl, 'vendor image', 'vendor.img', 50);
      fs.renameSync(this.tmpDir(), this.imagesDir());
      hilog.info(DOMAIN, TAG, 'images in place: %{public}s', this.imagesDir());
      report('done', 'Checking the images…', -1);
    } catch (e) {
      const msg = (e as Error).message ?? JSON.stringify(e);
      hilog.error(DOMAIN, TAG, 'provisioning failed: %{public}s', msg);
      this.removeTree(this.tmpDir());
      report('error', msg, 0);
    } finally {
      this.busy = false;
    }
  }
}
