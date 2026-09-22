// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// The settings page's half of the conversation with the root side.
//
// An app can not set a persist param — persist.waydroid.* is root-only and
// must stay that way (the .para.dac route bricked a device once) — so what the
// user asks for is written into a file in THIS app's storage, and the
// supervisor, which already reads this directory for the images, applies it:
//
//   seq 7                bumped on every change; a change is applied once
//   run on|off|restart   Start / Stop / Restart
//   enabled 0|1          -> persist.waydroid.enabled    (the master switch)
//   autostart 0|1        -> persist.waydroid.autostart
//   idle_stop <seconds>  -> persist.waydroid.idle_stop_s
//
// What comes back is the STATUS event (see EntryAbility), whose key=value
// tokens are the params AS THEY REALLY ARE.  Those win over our own file when
// the next change is written: a value set by hand from a shell must not be
// reverted by a stale copy of ours.
//
// The file is written through a rename, so the supervisor — which reads it
// every 2 s, as root, without any locking — never sees half of one.

import fs from '@ohos.file.fs';
import hilog from '@ohos.hilog';

const DOMAIN = 0x2500;   // 16 bits for an app, or the line is dropped
const TAG = 'waydroid.control';

// The applied state, from the supervisor's STATUS.  -1 / '' = not heard yet.
export const KEY_APPLIED_SEQ = 'wdAppliedSeq';
export const KEY_ENABLED = 'wdEnabled';          // 0 | 1 | -1
export const KEY_AUTOSTART = 'wdAutostart';      // 0 | 1 | -1
export const KEY_IDLE = 'wdIdle';                // seconds, -1 unknown
export const KEY_IMG_LOC = 'wdImgLoc';           // '' | none | app | data
export const KEY_IMG_BYTES = 'wdImgBytes';
// What WE want, mirrored so the UI reacts before the root side confirms.
export const KEY_WANT_RUN = 'wdWantRun';         // 'on' | 'off'
// The seq we last wrote.  A STATUS older than this describes the world before
// the request and must not be believed — most of all a `stopped` that was
// published a moment before the user pressed Start.
export const KEY_SENT_SEQ = 'wdSentSeq';

export const RUN_ON = 'on';
export const RUN_OFF = 'off';
export const RUN_RESTART = 'restart';

export class Settings {
  seq: number = 0;
  run: string = RUN_ON;
  enabled: number = -1;
  autostart: number = -1;
  idleStop: number = -1;
}

// One request: every field left at its "unset" value keeps what is there.
export class Change {
  run: string = '';
  enabled: number = -1;
  autostart: number = -1;
  idleStop: number = -1;
}

function num(v: string, fallback: number): number {
  const n = Number.parseInt(v, 10);
  return Number.isNaN(n) ? fallback : n;
}

export class Control {
  private path: string;
  private tmp: string;

  constructor(filesDir: string) {
    this.path = filesDir + '/control';
    this.tmp = filesDir + '/control.tmp';
  }

  read(): Settings {
    const s = new Settings();
    try {
      if (!fs.accessSync(this.path)) {
        return s;
      }
      const lines: string[] = fs.readTextSync(this.path).split('\n');
      for (const line of lines) {
        const t: string = line.trim();
        const i: number = t.indexOf(' ');
        if (i <= 0) {
          continue;
        }
        const k: string = t.substring(0, i);
        const v: string = t.substring(i + 1).trim();
        if (k === 'seq') {
          s.seq = num(v, 0);
        } else if (k === 'run') {
          s.run = v;
        } else if (k === 'enabled') {
          s.enabled = num(v, -1);
        } else if (k === 'autostart') {
          s.autostart = num(v, -1);
        } else if (k === 'idle_stop') {
          s.idleStop = num(v, -1);
        }
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'control unreadable: %{public}s', JSON.stringify(e));
    }
    return s;
  }

  // Our file, corrected by what the root side last reported.
  private current(): Settings {
    const s: Settings = this.read();
    const en: number = AppStorage.get<number>(KEY_ENABLED) ?? -1;
    const as: number = AppStorage.get<number>(KEY_AUTOSTART) ?? -1;
    const idle: number = AppStorage.get<number>(KEY_IDLE) ?? -1;
    const seq: number = AppStorage.get<number>(KEY_APPLIED_SEQ) ?? 0;
    if (en >= 0) {
      s.enabled = en;
    }
    if (as >= 0) {
      s.autostart = as;
    }
    if (idle >= 0) {
      s.idleStop = idle;
    }
    if (seq > s.seq) {
      s.seq = seq;        // somebody else's copy of the file got further
    }
    return s;
  }

  // Ask for something.  Returns what the file now says.
  apply(change: Change): Settings {
    const s: Settings = this.current();
    if (change.run !== '') {
      s.run = change.run;
    }
    if (change.enabled >= 0) {
      s.enabled = change.enabled;
    }
    if (change.autostart >= 0) {
      s.autostart = change.autostart;
    }
    if (change.idleStop >= 0) {
      s.idleStop = change.idleStop;
    }
    s.seq = s.seq + 1;
    this.write(s);
    AppStorage.setOrCreate<number>(KEY_SENT_SEQ, s.seq);
    // `restart` is a one-shot; as far as the UI is concerned we want it up.
    AppStorage.setOrCreate<string>(KEY_WANT_RUN, s.run === RUN_OFF ? RUN_OFF : RUN_ON);
    hilog.info(DOMAIN, TAG, 'seq %{public}d: run=%{public}s en=%{public}d as=%{public}d idle=%{public}d',
      s.seq, s.run, s.enabled, s.autostart, s.idleStop);
    return s;
  }

  // What the user last asked for, for a UI that opens before any STATUS.
  loadWanted(): void {
    const s: Settings = this.read();
    AppStorage.setOrCreate<string>(KEY_WANT_RUN, s.run === RUN_OFF ? RUN_OFF : RUN_ON);
  }

  private write(s: Settings): void {
    let text: string = 'seq ' + s.seq.toString() + '\n' + 'run ' + s.run + '\n';
    if (s.enabled >= 0) {
      text += 'enabled ' + s.enabled.toString() + '\n';
    }
    if (s.autostart >= 0) {
      text += 'autostart ' + s.autostart.toString() + '\n';
    }
    if (s.idleStop >= 0) {
      text += 'idle_stop ' + s.idleStop.toString() + '\n';
    }
    try {
      const file = fs.openSync(this.tmp, fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.WRITE_ONLY);
      try {
        fs.writeSync(file.fd, text);
        fs.fsyncSync(file.fd);
      } finally {
        fs.closeSync(file);
      }
      if (fs.accessSync(this.path)) {
        fs.unlinkSync(this.path);
      }
      fs.renameSync(this.tmp, this.path);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'control unwritable: %{public}s', JSON.stringify(e));
    }
  }
}
