# Android Apps — OHOS front-end for the Waydroid container (W5)

A minimal ArkUI system app (`org.oniroproject.androidapps`) that gives the
Waydroid Android container a real OHOS window. It hosts one fullscreen
XComponent (SURFACE) and hands its surface to `waydroid_compositor` over
the session SA (`../src/waydroid_session*.cpp`, SAID 9601), so the
container composites straight into this app's window — the same zero-copy
frame path as the W3 self-drawing node, but now owned by a windowed app
that respects focus, rotation and recents. Touch on the XComponent is
forwarded to the container; the surface lifecycle drives a cgroup2
freezer so a hidden Android session costs ~0 CPU.

## Why in-tree gn (not hvigor)

The native module (`entry/src/main/cpp/napi_init.cpp`) must reach the
session SA, which means linking the **inner** `samgr_proxy` /
`graphic_surface` APIs. There is no NDK `GetSystemAbility`, so a
hvigor-built (NDK-only) HAP cannot reach the compositor. The app is
therefore a gn `ohos_hap` with the native code as a bundled
`ohos_shared_library` (`shared_libraries`). The XComponent surface's
producer is resolved locally (`OH_NativeWindow_GetSurfaceId` +
`SurfaceUtils::GetSurface` in this process) and only the producer object
crosses to the compositor — the same pattern the camera preview uses.

## Data flow

```
XComponent(SURFACE, libraryname "androidapps")
  → OnSurfaceCreated(window)                     [napi_init.cpp]
      OH_NativeWindow_GetSurfaceId → SurfaceUtils::GetSurface
      → surface->GetProducer()->AsObject()
      → IWaydroidSession::SetForeground(true)     (thaw container)
      → IWaydroidSession::SetOutputSurface(producer)
  → DispatchTouchEvent → IWaydroidSession::InjectTouch(...)
  → OnSurfaceDestroyed → ClearOutputSurface + SetForeground(false) (freeze)
```

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
hdc shell "param set waydroid.input.grab 0"   # XComponent provides touch
hdc shell "aa start -a EntryAbility -b org.oniroproject.androidapps"
```

## Status & the open native-module-load problem

Builds, signs (system app), installs (`bm install` → "install bundle
successfully"), and **launches**: the window `androidapps0` (id 41) comes
up fullscreen and focused. But the bundled native module never runs —
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

Both say the "app native module links inner samgr/surface/ipc" shape
fights the app sandbox even for a system app. Two ways forward:

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
