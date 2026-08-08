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
   (`halium-blobs/waydroid/patches/libui.so`, bound by waydroidd) that
   clears usage to its low 24 bits — the clean descriptor the A14 mapper
   wants — instead of rejecting. See `patches/README`.

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

## Deployment (baked into waydroidd + graft)

`waydroidd` binds the libui patch, the gpu.xml overlay, and the graft dir
(`/data/waydroid/graft` → container `/odm`). The graft carries
`libbinderflags_shim.so`, `etc/init/waydroid-gralloc.rc`,
`etc/vintf/manifest/waydroid-allocator-aidl.xml`, and
`start-allocator.sh`. A fresh `waydroidd` start now boots straight to the
launcher with no manual steps.

## Building the shim

```sh
SDK=/home/mrfrank/setup-ohos-sdk/linux/23/native
$SDK/llvm/bin/clang --target=aarch64-linux-android -c binderflags_shim.S -o binderflags_shim.o
$SDK/llvm/bin/ld.lld -shared -soname libbinderflags_shim.so --allow-shlib-undefined \
    -o libbinderflags_shim.so binderflags_shim.o
```

## Known follow-ups (not blocking the launcher)

* Frame pacing: the hwc posts ~60 fps into RS; on a static screen the
  compositor logs `attach+flush failed: 41209000` (QUEUE_FULL) for the
  frames RS didn't consume — harmless (last good frame persists), but
  `wl_surface.frame` should follow RS flush-complete (W2 caveat).
* The allocator binary path (`mt6878`) is globbed but SoC-specific.
* Input (W4) is next: no touch reaches the container yet.
