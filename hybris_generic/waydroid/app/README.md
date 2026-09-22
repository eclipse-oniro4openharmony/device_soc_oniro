# Android Apps — the OHOS shell for Android apps

`org.oniroproject.androidapps`: an ArkUI system app that lists the Android apps
installed in the Waydroid container and opens each of them **in an ordinary
OHOS window** — OHOS status bar, OHOS gesture navigation, OHOS open/close
animations, one recents card per Android app. Android draws no bars of its own
(`../data/overlay/`), and nothing of Android is on the panel unless such a
window is. Plan and measurements:
`docs/hybris_generic/android_app_launcher_plan.md`.

```
 EntryAbility ─ pages/Index        the grid: icons + names from this app's own
                                   cache, refreshed when Android answers
 AppWindowAbility ─ pages/AppWindow   one instance per Android package
   (launchType "specified",           (stage/AppAbilityStage: key = package)
    key = package)                    XComponent(SURFACE) + splash
        │
        │ model/Container.ts:  globalThis.requireNapi('oniro.androidcontainer')
        ▼
 ../napi/  system NAPI module ──► SA 9601 (waydroid_compositor)
   attachWindow(surfaceId, package, w, h)   frames of THAT Android task → this surface
   sendTouch / sendKey(BACK) / setWindowActive
   listApps / getAppIcon / launchApp / closeApp   (libwdbinder → IPlatform)
```

## Why a system NAPI module

The window's XComponent surface has to reach the compositor, a separate root
process (its per-frame gralloc import needs `/android/vendor`, which app
sandboxes lack). That takes the inner `samgr` + `graphic_surface` APIs. A
native library **bundled in the HAP** cannot have them — tried in W5, failed
twice over: BMS leaves the entry module's `nativeLibraryPath` empty, and the
app's linker namespace refuses the inner libraries anyway. A module under
`/system/lib64/module/` is opened with a plain `dlopen()` outside that
namespace, the same way AVPlayer's NAPI gets a surface across. The SDK does
not know the module, so it is fetched with `requireNapi()` from a `.ts` file
(ArkTS forbids the untyped global).

Any app can load the module. It grants nothing: every call ends in SA 9601,
which answers this bundle's HAP token (and root) only.

## The window

`pages/AppWindow` runs a small state machine off one timer rather than off
events, because every precondition can come and go: the stack may not exist
yet (this app's SHOW beat is what starts it on demand), Android may be
booting, the supervisor may have rebuilt the generation (a new compositor
forgets its windows). Each tick makes true what should be: a live window for
the surface, the app launched into it, and — if no frame of it arrived within
3 s — launched again (the hwc lets Android's display sleep while nothing is
shown, and a launch that finds it asleep only wakes it).

* The window's content area is what Android's display becomes — the compositor
  configures the hwc with it — so Android lays out for exactly the space
  between the OHOS bars (1080×2199 on the Plinius: 117 px status bar, 84 px
  gesture bar).
* Touch: XComponent `onTouch` → `sendTouch` (multi-touch, px). The OHOS back
  gesture → `onBackPress` → `sendKey(158)` = Android BACK.
* A splash (icon + name, "Starting Android…" on a cold start) covers the
  surface until the compositor reports that app's own first frame
  (`ontop <package>`), so the previous app's last frame is never shown.
* Which window is in front is reported (`setWindowActive`): the compositor
  activates exactly that task's window, which is what makes the hwc present
  it and focus it in Android.

## When a window closes

`AppWindowAbility` listens to the compositor's
`org.oniroproject.waydroid.TASK` events (sent to this bundle only):

| Event | Reaction |
|---|---|
| `ontop <mine>` | drop the splash |
| `inactive <mine>` while in front | the user backed out of the app's last screen → `terminateSelf()` |
| `ontop <other>` while in front, and `<other>` has **no** window | Android opened a task of its own (a picker, a share target) → it gets a window, this one goes to the background |
| `gone <mine>` | the task ended → `terminateSelf()` |

Swiping the card away in recents (or any other end of the ability) calls
`closeApp(package)`; the hwc removes the Android task, so nothing keeps
running behind the user's back. Tasks that already have a window are never
switched by us — reacting to those made two open windows fight for the top
for ever.

The recents card carries the app's **name** but not its icon: SceneBoard
keeps one updated mission icon per *bundle*, so every card would show the icon
of the app opened last. Per-app icons need per-app bundles (integration plan,
phase A4).

## The lease (start on demand, freeze, crash safety)

`model/Lifecycle.ets`. While anything of ours is in the foreground the app
publishes `org.oniroproject.waydroid.SHOW` every 2 s; when nothing has been
for 400 ms, `HIDE`. The compositor treats it as a lease (10 s): thawed while
it holds, frozen (cgroup freezer, ~0 CPU) when it does not.

* **start on demand** — nothing of the stack runs until this app is opened;
  the first SHOW makes samgr start it (`../sa_profile/9601.json`). Opening the
  grid is usually enough head start for Android to be up by the first tap.
* **recovery** — a generation rebuilt by the supervisor is picked up by the
  next tick; an open window re-attaches and relaunches by itself (seen: the
  90 s first-frame watchdog firing under an open window).
* **safety** — if this app dies, the container freezes when the lease runs
  out, and the dead window is detached through its producer's death recipient.

It is counted across abilities and lives in `AppStorage`: the in-tree ets
build bundles every ability and page separately, so a module variable exists
once per bundle.

Measured on the Plinius (2026-09-21): tap → first frame of the app **0.5 s**
when it is already running, **0.9 s** when it is not, **~21 s** from a fully
dormant stack (one tap; the splash says "Starting Android…").

## The debug overlay

How Android was shown before it moved into windows — a fullscreen
self-drawing node above everything with a global touch grab — still exists
for bring-up without any app: `param set waydroid.debug.overlay 1`, then
restart the generation. It paints over the lock screen, which is why it is
not the default; this app's EXIT subscriber only serves its Vol-Down+Vol-Up
chord.

## Talking back: STATUS, and first-run provisioning

This app can read nothing of the stack (no params, no `/data/waydroid`), so the
supervisor tells it: `org.oniroproject.waydroid.STATUS`, data =

| data | the page shows |
|---|---|
| `disabled` | "Android apps are turned off on this device" (`persist.waydroid.enabled` ≠ 1 — device policy, not ours to change) |
| `needs-images <sys-url> <sha> <ven-url> <sha>` | the one-time download offer |
| `verifying` | "Checking the Android images…" |
| `bad-images <same four>` | "failed verification" + *Download again*; the page also deletes its copy |
| `starting` / `running` | the spinner / nothing (the container covers the page) |

`model/Provisioner.ets` downloads the two zips (`@ohos.request`), unpacks them
(`@ohos.zlib`) into `filesDir/images.tmp` and renames that to `filesDir/images`
at the very end, so the supervisor never sees half a file. 946 MB down, 2.07 GB
on the device, ~3 min on a good link. Storing them with the app is the point:
**uninstalling "Android Apps" gives the space back** (measured: 2.03 GB within
5 s, with Android running at the time — the supervisor notices the images are
gone and tears down, and the loop devices are auto-clear).

The app is not trusted with them. What to fetch comes from the supervisor (which
reads `images.manifest` from the system image); and `waydroidd` only mounts an
image through `verify_image()`: open once → root-owned + 0444 → a read lease
(refused by the kernel while anyone still has the file open for writing) →
sha256 against the manifest → loop-attached from that same fd. Nothing is
verified here; a bad download comes back as `bad-images`.

Two things that cost time, for whoever touches this next:

* the in-tree (non-hvigor) ets build **bundles the ability and every page
  separately**, so a static or module-level variable exists once per bundle —
  the page's copy of something the ability set is `undefined`. `AppStorage` is
  what they really share;
* an app's hilog domain is **16 bits**. The `0xD002500` this app used to log
  with is silently dropped, which is why it never appeared to log anything.

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

## Install & run

Preinstalled, as a **removable** app, on any image built with
`hybris_generic_soc_feature_waydroid` (`vendor/oniro/hybris_generic/
preinstall-config`). BMS only rescans that list on a fresh `/data`, so after
flashing over an existing one update it by hand — the app and the compositor
must come from the same image, they share the heartbeat protocol:

```sh
hdc shell "bm install -r -p /system/app/org.oniroproject.androidapps/AndroidApps.hap"
hdc shell "aa start -a EntryAbility -b org.oniroproject.androidapps"
```

`utils/host/waydroid-deploy.sh` (board repo) does both.

## Status

Verified on the Plinius, 2026-09-21 (Android 16 image): grid from cache and
from Android; Calculator, Clock and Settings each in their own window with
touch and BACK; round-robin switching between three windows, state kept;
backing out closes the window and ends the task; swiping a recents card away
ends the task; cold start from a dormant stack with one tap; recovery of an
open window across a supervisor rebuild; provisioning as before.
