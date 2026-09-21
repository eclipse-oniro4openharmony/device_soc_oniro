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
EntryAbility.onForeground → publish('org.oniroproject.waydroid.SHOW'), then again every 2 s
EntryAbility.onBackground → stop the beat, publish('org.oniroproject.waydroid.HIDE')
                                       │  (custom CommonEvent — no permission needed)
              ┌────────────────────────┴───────────────────────────┐
              ▼ stack not running                                  ▼ stack running
samgr: on-demand policy of SA 9601             compositor: VisibilityReceiver
  (sa_profile/9601.json) → init starts           → WaydroidSessionStub::ApplyVisibility
  waydroid_supervisor "9601#…" → compositor        visible : SetNodeVisible(true) + thaw + grab=1
  + container; the NEXT beat reveals them          hidden  : freeze + SetNodeVisible(false) + grab=0
```

SHOW is a **heartbeat** and visibility is a **lease** (10 s,
`kVisibilityLeaseMs`). A SHOW when already visible only renews the lease, so
the beat is free. That single decision gives:

* **start on demand** — nothing of the stack runs until this app is opened
  (unless `persist.waydroid.autostart=1`); the app needs no native code and no
  permission to make that happen;
* **recovery** — a generation rebuilt by the supervisor comes up hidden and is
  revealed by the next beat, with no guessing on the supervisor's side;
* **safety** — if this app dies without saying HIDE, the compositor hides,
  freezes and releases touch when the lease runs out, instead of leaving a
  fullscreen touch-grabbing layer over OHOS until reboot.

The exit chord (Vol-Down + Vol-Up) hides immediately and ignores SHOW for 3 s
(`HideAndHold`), the time this app needs to receive EXIT and background itself.

ApplyVisibility is mutex-serialized (concurrent visibility flips otherwise
stranded grab=1 over a frozen container → total touch loss). Hide/show toggles
node *visibility* rather than destroying it, so the container's last frame is
retained (a fresh node is black until the container redraws). The compositor
only listens to SHOW/HIDE **published by this bundle**
(`SetPublisherBundleName`) — SHOW puts a fullscreen, touch-grabbing layer over
OHOS and any app can publish a custom event — so `cem publish` from a shell no
longer drives it; use the app, or `waydroid_session_test`.
`entry/src/main/cpp/napi_init.cpp` is retained for the future XComponent path
but is no longer built.

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

Verified on the Plinius, 2026-09-21 — provisioning: with no images on the
device the app offered the download, fetched and unpacked both, the supervisor
went `verifying` → `starting` and Android came up from files in this app's
storage; a byte flipped in one of them → `bad-images`, copy deleted, *Download
again* worked; uninstalling gave the 2 GB back. Lifecycle: opening the app on a device with nothing
of the stack running → samgr starts the supervisor within 1 s → revealed by the
next beat → first frame ~10–12 s → Android booted ~15–20 s. Backgrounding
freezes and releases touch; killing the app in the foreground → the lease hides
the container 10 s later; 0 processes after the idle stop or a disable.
