# Android apps on hybris_generic (Waydroid)

Android apps running in ordinary OHOS windows on the Volla Phone Plinius
(`ansuz`). A [Waydroid](https://waydro.id) Android 16 (LineageOS 23) container
runs next to OHOS; its hardware composer talks Wayland to
`waydroid_compositor`, which hands each Android task's GPU buffers, without a
copy, to the OHOS window that the **Waydroid** front-end app opened for it. The
OHOS status bar, gesture navigation, animations and recents stay in charge;
Android draws no bars of its own.

Verified on the Plinius only. The X23 builds the same image, but the stack has
not been brought up there.

```
 OHOS app "Waydroid"  ──SHOW heartbeat──►  samgr starts the supervisor on demand
 (org.oniroproject.androidapps)            │
   one OHOS window per Android app         ▼
   XComponent surface ──NAPI──►  waydroid_compositor (SA 9601, Wayland server)
                                     ▲         │ buffers → RenderService
                       wayland/wlegl │         ▼
                                 container hwc (Android 16, Waydroid images)
                                     ▲
                       waydroidd: verifies + mounts the images, binderfs,
                       namespaces, cgroup freezer, NAT + DHCP, PulseAudio→OHOS
```

## What lives where

Code and configuration ship in the image, behind one product feature. `/data`
holds only data.

| Piece | Home |
|---|---|
| compositor, session SA + on-demand profile, `waydroidd`, `wdctl`, `waydroid_tcprelay`, `waydroid_audio`, supervisor + init cfg, gralloc graft, container config, front-end HAP | this directory, built only with `hybris_generic_soc_feature_waydroid` ([`waydroid.gni`](waydroid.gni)) |
| the product switch (off by default) | `vendor/oniro/hybris_generic/config.json` |
| init securebits exemption for the supervisor | `device/board/oniro/system_patch/patches/base/startup/init/0003-…` |
| host scripts: fetch/push images, deploy, adb into the container | `device/board/oniro/hybris_generic/utils/host/waydroid-*.sh` |
| `server_wlegl` build wiring | `third_party/libhybris`, upstream `OpenHarmony-6.1-LTS` |
| libwayland 1.23.1, libffi 3.4.6 | `third_party/wayland`, `third_party/libffi`, from the public forks [frankplus/wayland](https://github.com/frankplus/wayland) and [frankplus/libffi](https://github.com/frankplus/libffi), branch `OpenHarmony-6.1-LTS`: the upstream release tag plus one commit that only adds the OpenHarmony build files |
| Android system/vendor images | Volla's Waydroid builds, downloaded per [`data/images.manifest`](data/images.manifest), sha256-verified, used unmodified |

Installed on the device:

```
/system/bin/{waydroid_compositor,waydroidd,wdctl,waydroid_tcprelay,waydroid_audio}
/system/etc/init/waydroid_compositor.cfg        supervisor service + jobs
/system/profile/waydroid_supervisor.json        samgr: start it on the SHOW event
/system/etc/waydroid/                           supervisor script, waydroid.prop,
                                                hosthals.xml, images.manifest, graft/
/system/lib64/module/oniro/libandroidcontainer.z.so   the front-end's NAPI module
/system/app/org.oniroproject.androidapps/       front-end, preinstalled, removable
```

Created at run time: `/data/waydroid/{data.img,rootfs,run,graft,*.log,*.pid}`.
The images live either in `/data/waydroid/images/`, pushed from a host, or in
the front-end's own storage after it downloads them.

## 1. Sync

Start from the Volla setup in the
[board README](https://github.com/eclipse-oniro4openharmony/device_board_oniro/blob/OpenHarmony-6.1-LTS/README.md#oniro-on-volla-phones-hybris_generic-target)
(prerequisites, Halium blobs, build, flash). The Oniro manifest already brings
every repo the stack needs except libwayland and libffi. Before `repo sync`,
add those two with the overlay manifest:

```bash
repo init -u https://github.com/eclipse-oniro4openharmony/manifest.git \
     -b OpenHarmony-6.1-LTS -m oniro.xml --no-repo-verify
mkdir -p .repo/local_manifests
curl -fsSL -o .repo/local_manifests/waydroid.xml \
     https://raw.githubusercontent.com/eclipse-oniro4openharmony/device_board_oniro/OpenHarmony-6.1-LTS/hybris_generic/utils/host/waydroid-local-manifest.xml
repo sync -c && repo forall -c 'git lfs pull'
```

Check the result: `repo manifest | grep -E 'wayland|libffi'` should show both
with `remote="frankplus" revision="OpenHarmony-6.1-LTS"`.

## 2. Build

Turn the feature on in `vendor/oniro/hybris_generic/config.json`. The first
two entries already exist with `"features": []`. Add the last two to the same
`subsystem` block as `libhybris`:

```jsonc
{ "component": "hybris_generic_soc",
  "features": [ "hybris_generic_soc_feature_waydroid = true" ] },
{ "component": "libhybris",
  "features": [ "libhybris_enable_wayland = true" ] },
{ "component": "wayland",  "features": [] },
{ "component": "libffi",   "features": [] }
```

It is off by default because a tree synced without the overlay has no
wayland/libffi, and the stock `hybris_generic` must still build. With it off,
none of this directory is built or installed.

Then build as the board README describes. Two things matter here:

* **`do_patch.sh` is required.** Patch `0003` exempts `androidd` and
  `waydroid_supervisor` from init's securebits lock. Without it the
  container's zygote fails `prctl(PR_SET_KEEPCAPS)` and Android reboot-loops
  every ~18 s.
* Never use `--build-only-gn`: its ninja rules have no ccache wrapper, so the
  next build is a cold one.

```bash
bash device/board/oniro/system_patch/do_patch.sh
./build.sh --product-name hybris_generic --ccache     # in the build container
bash device/board/oniro/hybris_generic/kernel/ansuz/build_super_img.sh
bash device/board/oniro/hybris_generic/kernel/ansuz/build_init_boot_chainload.sh
bash device/board/oniro/hybris_generic/utils/host/flash-native.sh -d ansuz
```

`flash-native.sh --only super` is enough when only system/vendor changed, and
it leaves `/data` alone, including a provisioned `/data/waydroid`.

## 3. Provision

There are two ways. Both need about 2 GB free on `/data`.

**From the phone, no host.** Open **Waydroid**. If it says Android apps are
turned off, tap *Turn on*. It then offers to download the two images (946 MB of
zips, about 2 min over a good WiFi link), unpacks them into its own storage
(2.07 GB) and starts Android. Uninstalling the app gives the space back.

**From a host,** with `hdc` connected:

```bash
bash device/board/oniro/hybris_generic/utils/host/waydroid-deploy.sh
#   --no-images   the images are already on the device
#   --no-verify   don't open the app and wait for Android to boot
#   --autostart   start the container at boot instead of on demand
#   --disable     switch it off again (keeps the data)
```

The script is idempotent: about 7 min the first time (it pushes 2 GB) and under
a minute after that. It compiles and patches nothing. It fetches and verifies
the pinned images, pushes them, sets `persist.waydroid.enabled=1` and the start
policy, reinstalls the front-end from `/system/app`, then opens it and waits
for `sys.boot_completed`.

Whichever way the images arrive, `waydroidd` only mounts an image that matches
its `*.img` pin in `images.manifest`. It first makes the file root-owned and
read-only, and checks that nothing still has it open for writing. A rejected
pair reaches the app as `bad-images`, and the app offers the download again.

### Start policy

| | on demand (default) | `persist.waydroid.autostart=1` |
|---|---|---|
| at boot | the supervisor checks the policy and asks init to stop it; nothing of the stack stays resident | compositor and container come up, hidden |
| opening the app | its SHOW event makes samgr start the supervisor; ~20 s to a usable Android | instant |
| in the background | container frozen (cgroup v2); torn down after `persist.waydroid.idle_stop_s` (default 1800, 0 = never) | frozen, kept |
| `persist.waydroid.enabled` ≠ 1 | nothing runs; the app shows *Turn on* | same |

The front-end can't write `persist.*` params. It writes a control file into its
own storage, and the supervisor applies it as root. The gear panel in the app
covers start/stop/restart, the master switch, autostart, the idle timeout and
the images. See [`app/README.md`](app/README.md).

## 4. Verify

| What | How | Expected |
|---|---|---|
| dormant | after a reboot, before opening the app: `hdc shell "ps -ef \| grep -i waydroid"` | nothing (on-demand mode) |
| container up | open the app, then `hdc shell "cat /data/waydroid/supervisor.log"` | `started on demand`, `container init pid=N up; monitoring`, `first frame after ~Ns` |
| Android booted | `hdc shell "wdctl getprop sys.boot_completed"` | `1`, ~20 s after the start |
| apps | `hdc shell "wdctl list"` | the launcher apps of the image |
| pixels | tap an app in the grid | the Android app in an OHOS window, OHOS bars above and below |
| touch / back | tap in the app; swipe back from the edge | Android reacts; backing out of the last screen closes the window |
| lifecycle | go home | `/sys/fs/cgroup/waydroid/cgroup.events` shows `frozen 1` |
| network | `hdc shell 'chroot /proc/$(cat /data/waydroid/container.pid)/root /system/bin/ping -c 3 1.1.1.1'` | replies (host WiFi must be on) |
| switch off | `waydroid-deploy.sh --disable` | no waydroid process left within ~15 s |

For Android-side debugging, `utils/host/waydroid-adb.sh` (board repo) gives you
`adb` into the container.

## Directory map

| Path | What |
|---|---|
| `src/`, `include/` | `waydroid_compositor`: Wayland server (`wl_server`), wlegl buffer import, per-task output surfaces, session SA 9601 |
| `launcher/waydroidd.c` | the container launcher: image verification, mounts, binderfs, namespaces, cgroup |
| `launcher/tcprelay.c` | TCP relay for adb into the container |
| `cfg/` | init service + `waydroid-supervisor.sh`: the lifecycle (gate, on-demand start, idle stop, watchdog, graft assembly, control file) |
| `sa_profile/` | samgr profile that starts the supervisor on the front-end's SHOW event |
| `binder/` | `libwdbinder` (a libc-only binder client to the container's `IPlatform`) and `wdctl` |
| `napi/` | `oniro.androidcontainer`, the system NAPI module the front-end loads |
| `app/` | the **Waydroid** front-end HAP ([README](app/README.md)) |
| `audio/` | `waydroid_audio`: a minimal PulseAudio server that plays into an OHOS AudioRenderer |
| `net/` | container networking: veth, NAT, DHCP, uplink routing |
| `gralloc_bridge/` | starts the host's Mali AIDL allocator inside the container ([README](gralloc_bridge/README.md)) |
| `data/` | `images.manifest`, `waydroid.prop`, `hosthals.xml`, resource overlays ([README](data/README.md)) |
| `provision/` | container-side rc run once Android has booted: retires the setup wizard, takes Android's own volume control out of the audio path |
| `test/` | `waydroid_session_test`, `waydroid_wlegl_harness` |

## Known issues

* **WiFi sometimes comes up off after a reboot** (a device quirk, not
  Waydroid). The container then has no network, and the in-app download waits
  silently. Turn WiFi on in Settings.
* **No video decoders in the container.** The Android 16 image registers audio
  codecs only, so apps play a video's sound over a frozen picture.
* **Changing the image pin needs a fresh container `/data`.** Delete
  `/data/waydroid/data.img`; `waydroidd` recreates it.
* **Flashing over an existing `/data` doesn't update the front-end.** BMS only
  rescans preinstalls on a fresh `/data`. The app and the compositor share a
  protocol, so run `bm install -r -p
  /system/app/org.oniroproject.androidapps/AndroidApps.hap` (the deploy script
  does).
* **A start can wedge silently** (rare): the container's composer blocks in a
  Wayland roundtrip and nothing reaches the panel. The supervisor rebuilds any
  start that hasn't shown a first frame within 90 s.
* **Don't `nsenter` into the container from `hdc shell`.** A hung `nsenter -p`
  uses up hdcd's channels until a reboot. Use `wdctl`, or
  `chroot /proc/<init pid>/root <tool>` (`getprop`, `logcat -d` and `ping`
  work; anything that goes through `cmd`, such as `pm`, `wm` or `input`,
  hangs).
* **Any app can *start* the stack** (samgr's on-demand start can't filter
  publishers). Only the front-end can *show* it or send it input.
* The images come from `volla.tech/filedump`. If that location goes away, the
  pins in `images.manifest` still identify the exact builds, but a new mirror
  has to be found.
