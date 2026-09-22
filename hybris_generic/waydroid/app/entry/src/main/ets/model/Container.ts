// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
//
// The shell's native half, typed.  `oniro.androidcontainer` is a SYSTEM NAPI
// module (../../../../../napi/) that the SDK knows nothing about, so it can
// not be `import`ed — the loader would reject the name at build time.  It is
// fetched the way the loader's own output fetches every @ohos module:
// requireNapi().  This file is .ts, not .ets, because ArkTS forbids the
// untyped global that takes.

export interface AndroidApp {
  name: string;
  package: string;
}

export interface AndroidContainer {
  // -100 no compositor (the stack is not up yet), 0 booting, 1 ready, 2 frozen
  getState(): number;
  listApps(): Promise<AndroidApp[]>;
  getAppIcon(pkg: string): Promise<ArrayBuffer>;
  launchApp(pkg: string): Promise<number>;
  closeApp(pkg: string): number;
  // window id (> 0), or a negative code
  attachWindow(surfaceId: string, pkg: string, width: number, height: number): number;
  detachWindow(window: number): number;
  windowAlive(window: number): boolean;
  setWindowActive(window: number, active: boolean): void;
  // action: OHOS PointerEvent codes — 1 cancel, 2 down, 3 move, 4 up
  sendTouch(window: number, action: number, pointerId: number, x: number, y: number): void;
  // evdev code, pressed and released
  sendKey(window: number, code: number): void;
}

export const STATE_NO_COMPOSITOR = -100;
export const STATE_BOOTING = 0;
export const STATE_READY = 1;
export const STATE_FROZEN = 2;
export const KEY_BACK = 158;

// eslint-disable-next-line @typescript-eslint/no-explicit-any
const native = (globalThis as any).requireNapi('oniro.androidcontainer') as AndroidContainer;

export default native;
