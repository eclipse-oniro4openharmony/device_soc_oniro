# Waydroid gralloc bridge

The Waydroid container has to allocate real GPU buffers, because
`waydroid_compositor` forwards those same buffers to OHOS RenderService
without copying them. The only Mali gralloc on the device belongs to the
host (Halium 14, MediaTek mt6878), so the container has to reach it.

With the lineage-23 / Android 16 image that takes **one** thing, which is
what is left in this directory: the container's own VINTF declares no AIDL
HAL at all, so the host's AIDL Mali allocator has to be started inside the
container and declared there.

* `waydroid-gralloc.rc` — the init service that launches it, plus an
  `on property:sys.boot_completed=1` trigger that sets
  `waydroid.active_apps=Waydroid` to switch the hwc to full-UI mode once
  SurfaceFlinger is compositing a real framebuffer-target (earlier races a
  NULL-handle crash; see `../data/README.md`).
* `start-allocator.sh` — the launcher: Mali deps from `/vendor_extra`.
* `graphics-allocator-aidl.xml` — the VINTF fragment declaring the AIDL
  allocator. Without it the container's servicemanager rejects the
  registration with `EX_ILLEGAL_ARGUMENT` (-3).
* `/vendor/etc/gralloc/gpu.xml` — the Mali allocator aborts ("Unable to
  retrieve GPU capabilities") without it; `waydroidd` overlays it into the
  read-only shim `/vendor/etc` from `/vendor_extra`.

Android 16's libui uses **gralloc5**: it asks that AIDL allocator for its
mapper library suffix and then `dlopen`s `mapper.<suffix>.so`. The Volla
vendor image ships `/vendor/lib64/hw/mapper.mediatek.so` as a symlink to
`/vendor_extra/lib64/hw/mapper.mediatek.so`, so the container maps buffers
with the phone's own MTK mapper, in process. That is the whole client side.

## What this used to be, and why it shrank (2026-09-21)

Until the Android 16 image existed, the container was Android 13
(lineage-20) while the vendor blobs were Android 14, and that skew broke
gralloc4 across three gates. Two of them, and part of the third, are now
gone — the notes are kept because they say what to look at if a future
image regresses:

1. **A13 libui client-side usage validation** rejected usage bits the A14
   Mali mapper accepted (`-EINVAL`, "invalid usage bits"), and **the A14
   Mali mapper** then rejected `usage & 0xFFFE08282400` as "Invalid
   attributes". One 3-instruction patch to `libui.so` that cleared usage to
   its low 24 bits satisfied both. **Gone:** A16 takes the gralloc5 path
   through the host's own mapper, so there is no cross-version descriptor
   to validate.
2. **The allocator's one A14-only symbol.** `AServiceManager_addServiceWithFlags@LIBBINDER_NDK34`
   did not exist in the A13 container, and was trampolined to
   `AServiceManager_addService` by a hand-built `libbinderflags_shim.so`.
   **Gone:** the A16 `libbinder_ndk.so` exports it natively.
3. **The AIDL NDK interface libraries.** The A13 image had
   `allocator-V1-ndk` / `common-V3-ndk`, while the host allocator links
   `allocator-V2-ndk` / `common-V4-ndk`; the supervisor used to graft the
   host's `common-V5-ndk` in under the V4 soname. **Gone:** the A16 image
   ships `allocator-V2-ndk` and `common-V4-ndk` itself.

## Deployment (in the image; assembled per start)

Everything here ships in `system.img` under `/system/etc/waydroid/graft`
(`../BUILD.gn`: `waydroid_graft_*`): `start-allocator.sh`,
`etc/init/waydroid-gralloc.rc`,
`etc/vintf/manifest/waydroid-allocator-aidl.xml`, plus the net script and
its rc. Before every container generation the supervisor copies that tree
to `/data/waydroid/graft`; `waydroidd` binds it at the container's `/odm`
and overlays `gpu.xml`. Nothing is pushed from a host, and nothing in the
graft survives from one start to the next.

## Known follow-ups

* The allocator binary path (`mt6878`) is globbed but SoC-specific.
* Frame pacing: `wl_surface.frame` should follow RS flush-complete (W2
  caveat).
