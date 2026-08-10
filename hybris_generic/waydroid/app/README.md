# Android Apps — OHOS front-end for the Waydroid container (W5)

A minimal ArkUI system app (`org.oniroproject.androidapps`) that gives the
Waydroid Android container a launcher entry and a real OHOS app lifecycle.
It is a **pure-ArkUI launcher with no native module**: it draws nothing of
the container itself. The container's pixels reach the panel through the
compositor's self-drawing node (W3), and this app only **publishes SHOW /
HIDE common events** on fore/background. The compositor subscribes and
`WaydroidSessionStub::ApplyVisibility` shows+thaws or hides+freezes the
container, moving the W4 touch grab to match, so a hidden Android session
costs ~0 CPU and OHOS keeps the panel + touch when the app is away.
Verified end-to-end on device (2026-08-10): app foreground → container
shown+thawed+grabbed; app background → container hidden (node kept, last
frame retained) + frozen + touch released.

## Why pure-ArkUI (the earlier XComponent design and why it was dropped)

The original design hosted a fullscreen XComponent and handed its surface
producer to the session SA (SAID 9601) over binder, so the container
composited into the app's own window. That needs the app's bundled native
module to link the **inner** `samgr_proxy`/`graphic_surface` APIs — and
that fights the app sandbox two ways, both confirmed on device (see the
trace below): BMS leaves the entry module's `nativeLibraryPath` empty so
the XComponent `libraryname` load fails ENOENT, and even fixed, the app's
linker namespace forbids those inner libs. The pure-ArkUI design sidesteps
both: **zero inner libs in the HAP**, so it loads cleanly, and the
app→compositor channel is a **custom CommonEvent** (no permission, no param
DAC, no image-partition change — a DAC route was tried and abandoned: a new
`.para.dac` entry + the flash it needs briefly bricked the device, see
memory [[waydroid-w4-w5-input-frontend]]). The trade vs the XComponent design: the container draws on the
fullscreen overlay node rather than inside the app's own window surface —
fine for first usability; the windowed-surface version can come back later
if the sandbox barriers are solved. The session SA
(`../src/waydroid_session*.cpp`) is still published and its
SetOutputSurface/InjectTouch/SetForeground contract remains (proven via
`waydroid_session_test`) for that future path.

## Data flow

```
EntryAbility.onForeground → commonEventManager.publish('org.oniroproject.waydroid.SHOW')
EntryAbility.onBackground → commonEventManager.publish('org.oniroproject.waydroid.HIDE')
                                       │  (custom CommonEvent — no permission needed)
                                       ▼
compositor: VisibilityReceiver (CommonEventSubscriber)        [waydroid_compositor_main.cpp]
  → WaydroidSessionStub::ApplyVisibility(server, visible)     [waydroid_session.cpp]
      visible : Output().SetNodeVisible(true)  + FreezeContainer(false) + grab=1
      hidden  : FreezeContainer(true) + Output().SetNodeVisible(false)  + grab=0
```

Manual test / drive without the app: `cem publish -e org.oniroproject.waydroid.HIDE`
(or `.SHOW`). ApplyVisibility is mutex-serialized (concurrent
visibility flips otherwise stranded grab=1 over a frozen container → total
touch loss). Hide/show toggles node *visibility* rather than destroying it,
so the container's last frame is retained (a fresh node is black until the
container redraws). `entry/src/main/cpp/napi_init.cpp` is retained for the
future XComponent path but is no longer built.

## Build

Wired into `../BUILD.gn`'s `waydroid_group`, so it builds with the SoC
part. Standalone:

```sh
sudo docker exec -u root -w /home/openharmony/workdir oniro-build-6.1-lts \
  ./build.sh --product-name hybris_generic --ccache \
  --build-target device/soc/oniro/hybris_generic/waydroid/app:android_apps_hap
# -> out/hybris_generic/oniro_soc_products/hybris_generic_soc/AndroidApps.hap
```

## Signing (system app)

`signature/androidapps.p7b` is a provision profile for this bundle at
`apl=system_core`, `app-feature=hos_system_app` (see auto-memory
`oniro-haps-system-app-signing`) — without those the app installs but
`isSystemApp:false` and its SA calls are denied. Regenerate with the
oniro-haps recipe:

```sh
D=developtools/hapsigner/dist
sed -e 's/com.OpenHarmony.app.test/org.oniroproject.androidapps/g' \
    -e 's/"normal"/"system_core"/g' $D/UnsgnedReleasedProfileTemplate.json \
  | sed 's/"hos_normal_app"/"hos_system_app"/' > profile.json
java -jar $D/hap-sign-tool.jar sign-profile \
  -keyAlias "openharmony application profile release" -signAlg SHA256withECDSA \
  -mode localSign -profileCertFile $D/OpenHarmonyProfileRelease.pem \
  -inFile profile.json -keystoreFile $D/OpenHarmony.p12 \
  -outFile signature/androidapps.p7b -keyPwd 123456 -keystorePwd 123456
```

The in-tree `ohos_hap` default app cert (`OpenHarmonyApplication.pem`) is
the same OpenHarmony test CA as the profile cert, so the app signature
matches the profile.

## Install & run (side-load; container must be up)

```sh
hdc file send AndroidApps.hap /data/local/tmp/
hdc shell "bm install -p /data/local/tmp/AndroidApps.hap"
hdc shell "aa start -a EntryAbility -b org.oniroproject.androidapps"
# fore/background the app to show+thaw / hide+freeze; touch grab follows.
```

The `waydroid.app.visible` default + its app-writable DAC ship in the
image (`vendor/oniro/hybris_generic/etc/param/hybris_native.para{,.dac}`),
so a full build + flash is needed for the app's `setSync` to be permitted;
a side-loaded HAP on an image without the DAC launches but cannot flip the
param (the compositor side is still drivable by hand:
`param set waydroid.app.visible 0|1`).

## Status

**Pure-ArkUI launcher: builds, signs (system app), installs, and launches
cleanly — no native-module load error** (the ENOENT below is gone because
the HAP bundles no `.so`). The compositor's `ApplyVisibility`
(show/hide + freeze/thaw + grab) is verified by flipping the param by hand
(`cgroup.freeze` tracks it). End-to-end (app lifecycle → param → compositor)
needs the image flashed so the app's `setSync` passes the param DAC.

### Historical: why the XComponent design was dropped (the native-module blocker)

The earlier design's bundled native module never ran —
`dlopen_impl load library header failed for libandroidapps.so`,
`key:default/androidapps ... No such file or directory`. Two stacked
app-sandbox barriers:

1. **BMS did not associate the native lib with the entry module.**
   `bm dump` shows the *application* `nativeLibraryPath: libs/arm64`
   (cpuAbi arm64-v8a) but the *entry module* `nativeLibraryPath: ""`,
   `cpuAbi: ""`. So the napi module loader
   (`native_module_manager.cpp` GetNativeModulePath) has no app-lib path
   for the module, falls through to a bare `libandroidapps.so`, and gets
   ENOENT — it never tries the installed `.../libs/arm64/
   libandroidapps.z.so`. An in-tree `ohos_hap` + `shared_libraries`
   packs the `.so` under `libs/arm64-v8a/` but doesn't produce the
   per-module native-lib metadata hvigor does (likely a missing
   `pack.info` abi entry). This is the immediate blocker.
2. **Linker namespace.** Even once (1) is fixed, the module's NEEDED
   inner libs (`libsamgr_proxy`, `libsurface`, `libipc_single`,
   `libutils`) are not in an app's `ndk`/`moduleNs_default` namespace
   allow-list, so `dlopen` of the module would fail on its deps.

### Root cause traced (2026-08-09, on device)

Live `bm dump` confirms: application `nativeLibraryPath: "libs/arm64"`,
`cpuAbi: "arm64-v8a"`, but the **entry module** `nativeLibraryPath: ""`,
`cpuAbi: ""`, `isCompressNativeLibs: true`. The load fails with
`load libandroidapps.so failed ... errno=2` (ENOENT) in every namespace.
Traced through BMS:
`InnerBundleInfo::FetchNativeSoAttrs` (inner_bundle_info.cpp) falls back to
the **application** path when `compressNativeLibs && !isLibIsolated`, so the
lib IS resolvable in principle — but the XComponent `libraryname` load goes
through the napi module manager, which reads
`GetHapModuleInfo().nativeLibraryPath` (inner_bundle_info.cpp:1674 —
`hapInfo.nativeLibraryPath = it->second.nativeLibraryPath`, the raw, empty
*module* value, NOT the FetchNativeSoAttrs fallback). So the loader gets ""
and falls through to a bare `libandroidapps.so` → ENOENT. In-tree
`ohos_hap` + `shared_libraries` leaves the compressed module path empty; a
sibling in-tree HAP (`ringtone_extension_hap`) uses the same gn shape but
loads its lib via the *extension* framework, not `libraryname`, so it never
hits line 1674.

### Recommended path (cleanest, sidesteps BOTH blockers)

Make the app a **pure-ArkUI** launcher entry with **no native module**, and
reuse the W3 self-drawing node for display:

* App `onForeground`/`onBackground` (or page show/hide) sets a system param
  (`@ohos.systemParameterEnhance`, allowed for a system app), e.g.
  `waydroid.app.visible=1|0`.
* The **compositor** (already a system process with full namespace, already
  uses `WatchParameter` for `waydroid.input.grab`, and already owns the
  session freeze/thaw) watches that param: on 1 → attach/show the
  self-drawing node + thaw the container; on 0 → detach/hide + freeze.

This gives a launchable "Android Apps" icon with real lifecycle-driven
freeze/thaw, using only what already works, and puts **zero inner libs in
the HAP** — no nativeLibraryPath problem, no namespace problem. The trade
vs the XComponent design: the container draws on the fullscreen overlay
node rather than inside the app's own window surface (fine for first
usability; the windowed-surface version can come later if wanted).

The older "fix the packaging" routes remain, but both are deeper:

* **Fix the packaging** — get BMS to attribute the native lib to the
  entry module (pack.info abi, or the right module.json/gn wiring) AND
  widen the app namespace to the needed inner libs (system-app namespace
  config). Verifies the current design as-is.
* **Invert the connection** (cleaner) — the app HAP uses only the NDK
  (`OH_NativeWindow_WriteToParcel` gives the producer object; XComponent
  touch via ArkTS), and the *compositor* (already a system process with
  full namespace) reaches the app — e.g. the app hosts a small
  ServiceExtensionAbility the compositor connects to, or registers its
  producer through a system-provided channel. No inner libs in the HAP.

The SA / surface-handoff / touch / freeze contract the app drives is
complete and **verified** independently via `waydroid_session_test`
(same `WaydroidSessionProxy`), so only the app-sandbox loading is open.

## Other follow-ups

* Freeze-on-background: today freeze is driven by surface-destroyed, which
  fires on teardown, not on every background. A true background freeze
  needs an `onBackground` path.
* Cold-start spinner timing (`Index.ets`) is a fixed delay; ideally follow
  the compositor's first-frame signal.
