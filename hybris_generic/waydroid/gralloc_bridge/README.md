# Waydroid gralloc bridge (W3 — WORKING)

**The LineageOS 20 launcher renders on the Volla Plinius panel** through
this bridge (2026-08-08). The container is A13 (lineage-20) but the only
Mali gralloc on the device is the host's A14 (Halium 14, MTK mt6878).
A13-userland ↔ A14-vendor gralloc4 is incompatible across three gates;
this directory bridges all three so SurfaceFlinger can allocate GPU
buffers, composite, and forward frames to `waydroid_compositor`.

## The three gates and how each is bridged

1. **A13 libui client-side usage validation.** `Gralloc4Mapper::
   validateBufferDescriptorInfo` in the container's `/system/lib64/
   libui.so` rejects usage bits the A14 Mali mapper actually accepts,
   with `-EINVAL` ("invalid usage bits"). Fixed by a 3-instruction patch
   (derived by waydroidd on every start from `../data/patches.conf`) that
   clears usage to its low 24 bits — the clean descriptor the A14 mapper
   wants — instead of rejecting. See `../data/patches.conf`.

2. **A14 Mali mapper attribute check.** With gate 1 patched, the mapper's
   `createDescriptor` rejects `usage & 0xFFFE08282400` (bits 33-47, 27,
   21, 19, 13, 10) as "Invalid attributes". SF's requested `0x1b00` has
   none of those, so the same clean-usage patch from gate 1 satisfies
   this too (proven: `mali_gralloc` no longer logs "Invalid descriptorInfo
   sizes"/"Invalid attributes").

3. **No reachable allocator.** SF probes HIDL `allocator@4.0/3.0/2.0`
   (all absent from VINTF); the host Mali allocator is **AIDL**
   (`allocator-V2-service-mediatek`, in the androidd/A14 container) and
   `host_hwbinder`/`hosthals.xml` forward HIDL only. Fixed by **running
   the host A14 AIDL allocator inside the container**:
   * `binderflags_shim.S` — the A14 allocator's one A14-only libbinder_ndk
     symbol (`AServiceManager_addServiceWithFlags@LIBBINDER_NDK34`)
     trampolined to A13 `AServiceManager_addService` (weak import so the
     .so preloads harmlessly). Build: see below. Deployed to `/odm`.
   * `graphics-allocator-aidl.xml` — VINTF fragment declaring the AIDL
     allocator (else servicemanager rejects registration with
     `EX_ILLEGAL_ARGUMENT` -3). Placed in `/odm/etc/vintf/manifest/`.
   * `/vendor/etc/gralloc/gpu.xml` — the Mali allocator aborts ("Unable to
     retrieve GPU capabilities") without it; waydroidd overlays it into
     the read-only shim `/vendor/etc` from `/vendor_extra`.
   * `start-allocator.sh` + `waydroid-gralloc.rc` — the init service that
     launches the allocator (shim preloaded, Mali deps from
     `/vendor_extra`), plus an `on property:sys.boot_completed=1` trigger
     that sets `waydroid.active_apps=Waydroid` to switch the hwc to
     full-UI mode once SF is compositing a real framebuffer-target.

## Deployment (in the image; assembled per start)

Everything here ships in `system.img` under `/system/etc/waydroid/graft`
(`../BUILD.gn`: `waydroid_graft_*`): `libbinderflags_shim.so`,
`start-allocator.sh`, `etc/init/waydroid-gralloc.rc`,
`etc/vintf/manifest/waydroid-allocator-aidl.xml` (plus the net script and its
rc). Before every container generation the supervisor copies that tree to
`/data/waydroid/graft` and adds the two host AIDL graphics libs from
`/android/system` (common-V5 under the V4 soname); `waydroidd` binds the result
at the container's `/odm`, applies the libui patch from `../data/patches.conf`
and overlays gpu.xml. Nothing is pushed from a host, and nothing in the graft
survives from one start to the next.

## Building the shim

Built by GN (`waydroid_binderflags_shim_build` → `build_android_shim.py`): it is
an *Android* object loaded inside the container, so it can not be an
`ohos_shared_library`. The action runs the tree's clang with
`--target=aarch64-linux-android` and `ld.lld -shared --build-id=none`; the
source is position-independent assembly with one weak import, so neither step
needs a sysroot. The output is byte-identical to the one the old hand recipe
produced (sha256 `40593709…`).

## Known follow-ups (not blocking the launcher)

* Frame pacing: the hwc posts ~60 fps into RS; on a static screen the
  compositor logs `attach+flush failed: 41209000` (QUEUE_FULL) for the
  frames RS didn't consume — harmless (last good frame persists), but
  `wl_surface.frame` should follow RS flush-complete (W2 caveat).
* The allocator binary path (`mt6878`) is globbed but SoC-specific.
* Input (W4) is next: no touch reaches the container yet.
