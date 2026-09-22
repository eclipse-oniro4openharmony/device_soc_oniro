# Waydroid container configuration (tracked, shipped in the image)

Installed to `/system/etc/waydroid/` when the product builds Android app
support (`hybris_generic_soc_feature_waydroid`). Together with the binaries in
`/system/bin` this is the whole non-image half of the stack: a device needs
nothing else under `/data/waydroid` than the two upstream images.

The images are used **unmodified**. Up to lineage-20 two files of them had to
be patched at every container start (a `patches.conf` word table applied by
`waydroidd`); both patches were consequences of running an Android 13 userland
on Android 14 vendor blobs, and the lineage-23 / Android 16 image needs
neither — see `../gralloc_bridge/README.md`.

| File | What it is |
|---|---|
| `images.manifest` | sha256 pins for the two Waydroid OTA zips **and for the images inside them**, plus the download base. The `*.img` pins are a security boundary: the container's init runs as real root and the images may have been downloaded by an unprivileged app, so `waydroidd` refuses to mount an image that is not listed here (`verify_image()`, `waydroidd verify DIR`). The zip lines and `base-url` are what the supervisor hands to the front-end as the download source, and what `utils/host/waydroid-images.sh` (board repo) fetches from a host; both build the URL as `<base-url>/<zip>`. system = lineage-23.0 (Android 16) VANILLA arm64-only, vendor = HALIUM_16 arm64-only, from Volla's file dump. |
| `waydroid.prop` | the container's property file, bind-mounted over the shim vendor's placeholder. Mirrors upstream `make_base_props()` for a Halium MTK host, plus the ansuz identity, `ro.sf.lcd_density=480` and the ARM EGL block. `/data/waydroid/waydroid.prop`, if present, overrides it. |
| `hosthals.xml` | trimmed host-HAL passthrough list (plan D11): only allocator, mapper, memtrack, media.c2 and MediaTek mms. Every HAL that OHOS already owns a client of (camera, drm, power, thermal, vibrator, gnss, nfc…) is dropped to avoid double-client contention. `/data/waydroid/hosthals.xml`, if present, overrides it. |

Also here, because it is data too:

* `overlay/` — `OniroNoBars.apk`, a static resource overlay on the Android
  framework (no code, five values) that removes Android's status bar and
  navigation bar: Android apps run inside OHOS windows, under the OHOS bars.
  Installed into the graft (`/odm/overlay`), which Android scans at boot.
  Committed prebuilt; `overlay/build_rro.sh` rebuilds it against the running
  container's `framework-res.apk` — **do that after every image bump**: the
  build fails if a resource name is gone, instead of the bars silently coming
  back. The status bar is 1 px, not 0: on Android 16 a zero-height status bar
  makes the desktop-windowing caption steal every touch from the app (see the
  comment in `overlay/nobars/res/values/values.xml`).
* `../provision/waydroid-provision.rc` — disables LineageOS' setup wizard once
  Android has booted. An unfinished wizard holds the HOME role and puts itself
  in front of whatever is launched. From init inside the container because
  `pm` started from outside hangs (no caller identity).

Two `waydroid.prop` settings are load-bearing and were each paid for with a
boot failure:

* `ro.hardware.egl=mali` — with upstream's `meow` wrapper the container's
  zygote SEGVs in `eglGetDisplay` (null hooks).
* `waydroid.background_start=true`, and **no** `waydroid.active_apps` here —
  presetting `active_apps=Waydroid` makes the hwcomposer crash on
  SurfaceFlinger's first empty present (full-UI mode forwards a
  framebuffer-target layer whose handle is NULL until something
  client-composites). The switch to the full UI is a runtime prop flip after
  `sys.boot_completed`, which `waydroid-gralloc.rc` does.

To re-derive the prop file for a new device, follow upstream Waydroid's
`make_base_props()` and change the `ro.product.waydroid.*` identity block; the
EGL/gralloc lines above must stay as they are on a Halium MTK host.
