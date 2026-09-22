// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// The grid's memory.  The list of Android apps and their icons can only be
// asked of a container that is running AND thawed AND booted — seconds away
// on a cold start — while the grid has to be there the moment the app opens.
// So the last answer is kept in this app's own storage (filesDir/apps.json,
// filesDir/icons/<package>.png) and refreshed whenever Android answers.
// Uninstalling Waydroid removes it with everything else.

import fs from '@ohos.file.fs';
import hilog from '@ohos.hilog';
import container from './Container';
import type { AndroidApp } from './Container';

const DOMAIN = 0x2500;
const TAG = 'waydroid.cache';

export class AppEntry {
  name: string = '';
  pkg: string = '';
  iconUri: string = '';
}

class CachedApp {
  name: string = '';
  package: string = '';
}

export class AppCache {
  private dir: string;

  constructor(filesDir: string) {
    this.dir = filesDir;
  }

  private listPath(): string {
    return this.dir + '/apps.json';
  }

  iconPath(pkg: string): string {
    return this.dir + '/icons/' + pkg + '.png';
  }

  private entry(name: string, pkg: string): AppEntry {
    const e = new AppEntry();
    e.name = name;
    e.pkg = pkg;
    e.iconUri = fs.accessSync(this.iconPath(pkg)) ? 'file://' + this.iconPath(pkg) : '';
    return e;
  }

  load(): AppEntry[] {
    const out: AppEntry[] = [];
    try {
      if (!fs.accessSync(this.listPath())) {
        return out;
      }
      const apps = JSON.parse(fs.readTextSync(this.listPath())) as CachedApp[];
      for (const app of apps) {
        out.push(this.entry(app.name, app.package));
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'cache unreadable: %{public}s', JSON.stringify(e));
    }
    return out;
  }

  // Ask Android; on success the cache is rewritten and icons of apps we have
  // none for are fetched.  Rejects (with the container's code) when Android
  // cannot answer right now — the caller keeps showing what it has.
  async refresh(): Promise<AppEntry[]> {
    const apps: AndroidApp[] = await container.listApps();
    apps.sort((a: AndroidApp, b: AndroidApp) => a.name.localeCompare(b.name));
    const cached: CachedApp[] = [];
    for (const app of apps) {
      const c = new CachedApp();
      c.name = app.name;
      c.package = app.package;
      cached.push(c);
    }
    this.write(this.listPath(), JSON.stringify(cached));
    if (!fs.accessSync(this.dir + '/icons')) {
      fs.mkdirSync(this.dir + '/icons');
    }
    for (const app of apps) {
      if (fs.accessSync(this.iconPath(app.package))) {
        continue;
      }
      try {
        const png = await container.getAppIcon(app.package);
        this.write(this.iconPath(app.package), png);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'no icon for %{public}s', app.package);
      }
    }
    const out: AppEntry[] = [];
    for (const app of apps) {
      out.push(this.entry(app.name, app.package));
    }
    return out;
  }

  // Truncating: a shorter list must not keep the old one's tail.
  private write(path: string, data: string | ArrayBuffer): void {
    const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.WRITE_ONLY);
    try {
      fs.writeSync(file.fd, data);
    } finally {
      fs.closeSync(file);
    }
  }
}
