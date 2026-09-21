# Waydroid container configuration (tracked, shipped in the image)

Installed to `/system/etc/waydroid/` when the product builds Android app
support (`hybris_generic_soc_feature_waydroid`). Together with the binaries in
`/system/bin` this is the whole non-image half of the stack: a device needs
nothing else under `/data/waydroid` than the two upstream images.

| File | What it is |
|---|---|
| `images.manifest` | sha256 pins for the two Waydroid OTA zips **and for the images inside them**, plus the download base. The `*.img` pins are a security boundary: the container's init runs as real root and the images may have been downloaded by an unprivileged app, so `waydroidd` refuses to mount an image that is not listed here (`verify_image()`, `waydroidd verify DIR`). The zip lines and `base-url` are what the supervisor hands to the front-end as the download source, and what `utils/host/waydroid-images.sh` (board repo) fetches from a host. system = lineage-20.0 VANILLA arm64, vendor = HALIUM_13 arm64. No HALIUM_14 vendor image is published — the A13-image-on-A14-blobs skew is why the gralloc bridge and the two binary patches exist. |
| `patches.conf` | the two binary patches as a word table, with the reasoning. `waydroidd` applies it on every container start to the images it has just mounted; `utils/host/waydroid-patches.sh` replays it on the host and checks the sha256 pins. Bump it together with `images.manifest`. |
| `waydroid.prop` | the container's property file, bind-mounted over the shim vendor's placeholder. Mirrors upstream `make_base_props()` for a Halium-14 MTK host, plus the ansuz identity, `ro.sf.lcd_density=480` and the ARM EGL block. `/data/waydroid/waydroid.prop`, if present, overrides it. |
| `hosthals.xml` | trimmed host-HAL passthrough list (plan D11): only allocator, mapper, memtrack, media.c2 and MediaTek mms. Every HAL that OHOS already owns a client of (camera, drm, power, thermal, vibrator, gnss, nfc…) is dropped to avoid double-client contention. `/data/waydroid/hosthals.xml`, if present, overrides it. |

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
