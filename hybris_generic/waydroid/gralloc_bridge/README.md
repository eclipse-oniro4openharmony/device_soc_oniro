# Waydroid gralloc bridge (W3, WORK IN PROGRESS)

The A13 (lineage-20) Waydroid container has to allocate GPU buffers, but
the only Mali gralloc on the device is the host's **A14** (Halium 14, MTK
mt6878) stack. A13-userland ↔ A14-vendor gralloc4 is incompatible across
**three** gates (full write-up in the plan §9 W3). This directory holds
the reusable pieces of the in-progress bridge; **it does not yet produce
pixels** — gate 3 (the Mali mapper rejecting the A13 descriptor) is open.

## The three gates

1. **A13 libui client-side usage validation** — `Gralloc4Mapper::
   validateBufferDescriptorInfo` in the container's `/system/lib64/
   libui.so` rejects phantom usage bits (0x7f00000000 + bit 24) with
   `-EINVAL` ("invalid usage bits"). **Solved** by
   `halium-blobs/waydroid/patches/libui.so` (clears the bits, continues),
   bound by waydroidd.

2. **No reachable allocator** — SF probes HIDL `allocator@4.0/3.0/2.0`
   (all absent from VINTF); the host's Mali allocator is **AIDL**
   (`allocator-V2-service-mediatek`, in the androidd/A14 container) and
   `host_hwbinder`+`hosthals.xml` forward HIDL only. Running the host A14
   allocator binary inside the A13 container needs exactly one A14
   libbinder_ndk symbol, `AServiceManager_addServiceWithFlags@LIBBINDER_
   NDK34` — everything else it imports is base `@LIBBINDER_NDK` (present
   through NDK33 in the A13 lib). `binderflags_shim.S` here is that
   one-symbol trampoline (forwards to A13 `AServiceManager_addService`,
   dropping the flags arg). With it `LD_PRELOAD`ed the A14 allocator
   **links**, but does not yet register cleanly — needs an init service +
   the right vendor linker namespace, not a manual `nsenter` run.

3. **Mali mapper rejects the A13 descriptor (OPEN)** — with gate 1
   patched, SF's in-process A14 Mali mapper `createDescriptor` fails:
   `mali_gralloc: Invalid descriptorInfo sizes` / `Invalid attributes to
   create descriptor for Mapper 4.0`, `GraphicBufferAllocator ... : 3`.
   This is before the allocator is even called, so it must be fixed first.
   Likely the A13 `BufferDescriptorInfo` (or the usage the gate-1 patch
   left) doesn't satisfy the A14 Mali mapper's size/attribute
   computation. Needs RE of `mapper.mediatek.so`'s createDescriptor, or a
   different gate-1 patch that yields a descriptor the A14 mapper accepts
   (compare against what the A14 libui / libhybris path produces, which
   works — see the W2 harness `usage_hex` probe).

## Building the shim

```sh
SDK=/home/mrfrank/setup-ohos-sdk/linux/23/native
$SDK/llvm/bin/clang --target=aarch64-linux-android -c binderflags_shim.S -o binderflags_shim.o
$SDK/llvm/bin/ld.lld -shared -soname libbinderflags_shim.so --allow-shlib-undefined \
    -o libbinderflags_shim.so binderflags_shim.o
```

The import (`AServiceManager_addService`) is **weak** so the .so can be
preloaded into wrapper processes (init/nohup/timeout) that don't link
libbinder_ndk without failing; it binds to the real symbol only inside
the allocator, which needs libbinder_ndk regardless.

## What works today (do NOT regress)

Everything up to gate 3: the container boots to zygote64/AudioFlinger,
the hwc drives `waydroid_compositor`, and W2's zero-copy frame path
renders on the panel. Only SF's GPU composition (hence the launcher) is
blocked, on gate 3.
