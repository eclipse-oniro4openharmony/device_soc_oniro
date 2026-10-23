# Oniro x86_general Mesa GPU driver — provenance & build

The prebuilt GPU libraries in this directory (`libgallium_dri.so`,
`libEGL.so.1.0.0`, `libGLESv2.so.2.0.0`, `libGLESv1_CM.so.1.1.0`,
`libgbm.so.1.0.0`) are **Mesa 25.0.7**, cross-compiled from source for
`x86_64-linux-ohosmusl`. This file records where they come from and how to
rebuild them.

> History: the first `libgallium_dri.so` was an opaque **Mesa 22.2.5** blob
> built by contributor *diemit* in his private tree (RUNPATH
> `/home/diemit/ohos_60/...`); the originals are in `../lib64.orig-22.2.5/`.
> It was then rebuilt from diemit's published fork (gitcode
> `diemit/third_party_mesa3d` @ `135d43ab`), and is now built from that work
> rebased onto OpenHarmony-7.0-Release (below).

## Provenance chain

```
freedesktop.org Mesa 25.0.x                       (canonical upstream)
  └─> OpenHarmony mesa fork  (OHOS platform port + ohos/ build glue)
        github.com/eclipse-oniro-mirrors/third_party_mesa3d
        branch OpenHarmony-7.0-Release @ 339cb11 (in-tree: third_party/mesa3d, 25.0.1)
      └─> Oniro fork          (diemit's x86_64 port + Oniro fixes, rebased)
            github.com/frankplus/third_party_mesa3d
            branch OpenHarmony-7.0-Release @ 2d564becbaddf0ce464c66b8f6682aa8b80bd094
          └─> these binaries   (built from that source; see build recipe below)
```

The Oniro branch is upstream OpenHarmony-7.0-Release plus:

| Commit | Author | What |
|---|---|---|
| `0d725a0` | diemit | x86_64 build: `ohos/build_ohos86.py`, `meson_cross_process86.py`, pkg-config templates, reworked OHOS EGL platform, prebuilt static LLVM 15 / SPIRV libs (`ohos/llvm_libx86` etc.) |
| `7d2147f` | diemit | Mesa 25.0.1 → 25.0.7 |
| `3cb31e4` | diemit | README build path |
| `d09ff6c` | diemit | radv OHOS support (not built here) |
| `37b9b9c` | Oniro | `kms_sw`: lock the shared buffer list and reference counts |
| `0b1ba09` | Oniro | `util`: use the NEON memcpy only on ARM (x86_64 build fix) |
| `2d564be` | Oniro | x86_64 cross setup links the product's own libqos |

diemit's fork (gitcode `diemit/third_party_mesa3d`, branch
`mesa25.0.1_x86_dev`) shares no history with the OpenHarmony repository: its
root is a snapshot of OpenHarmony master at `ea74a63` (2025-07-25) plus his
changes. The first commit above is that difference replayed onto 7.0; see its
message for the one conflict (`platform_ohos.c`). The static libraries are
regular files in the Oniro branch: GitHub refuses new LFS objects in a public
fork, and none is over 24 MB.

The `kms_sw` fix: the winsys shared its buffer list, reference counts and plane
lists between threads with no lock. render_service uses GL from two threads
(the uni-render thread and the composer's client-composition redraw), and
importing a dma-buf already imported on the same fd returns the same GEM handle,
so both threads ref and unref one display target. The race destroyed targets
twice or freed them while in use: render_service crashed or deadlocked within
minutes of continuous redraws, and the restarts could escalate to a kernel
panic. The fix adds a winsys-wide lock and initialises `map_lock` for targets
imported from a dma-buf, which was never done.

## How these binaries were built

Full recipe: **`oniro_build_x86_general.sh`** (in this directory). It runs inside
the build container (`<container>`), reads the source from
`third_party/mesa3d-new`, and:
1. applies any `lib64/patches/*.patch` not yet in the source (none at present:
   the Oniro fixes are commits on the branch);
2. generates the `x86_64-linux-ohosmusl` meson cross-file + pkg-config `.pc`
   set (`ohos/meson_cross_process86.py`), repointing `zlib.pc` at the static
   `obj/third_party/zlib/libz.a` and `expat.pc` at
   `obj/third_party/expat/libexpat_static.a`;
3. `meson setup` with `-Dgallium-drivers=swrast,llvmpipe,softpipe,virgl
   -Dvulkan-drivers= -Dllvm=enabled -Dshared-llvm=disabled -Dlibunwind=disabled
   -Dunversion-libgallium=true` (→ output named `libgallium_dri.so`);
4. `ninja && ninja install`, then copies the five libraries here.

LLVM is provided by the tree's OHOS clang-15 (X86 target) plus diemit's prebuilt
static libs; meson sees it via the wrapper at `<tree>/clang-dev/bin/llvm-config`.

`libgallium_dri.so` needs `libqos.so` (OpenHarmony thread QoS, used since
upstream's "adjust zink thread priority"); the image provides it in
`/system/lib64/ndk`. On the emulator the kernel has no QoS node, so setting a
thread QoS fails and logs `Failed to set thread QoS` now and then; harmless.

### Deviation from diemit's stock recipe
diemit's `ohos/build_ohos86.py` also builds `radeonsi` + `radv` (AMD). Those need
an LLVM built with the **AMDGPU** target; the OHOS prebuilt clang-15 ships X86
only and diemit's matching private LLVM is unpublished, so they are dropped.
`virgl` (virtio-gpu) + `llvmpipe`/`softpipe` are what the QEMU emulator uses and
are what these binaries contain.

### Binary prebuilts used at link time (from diemit)
| Path in the source tree                 | Contents |
|-----------------------------------------|----------|
| `ohos/llvm_libx86/` (86 `.a`, 219 MB)   | static LLVM-15 target libs (X86 used; AMDGPU present, unused here) |
| `ohos/SPIRV-Tools/` (3 `.a`, 18 MB)     | SPIR-V tools |
| `ohos/SPIRV-LLVM-Translator/` (13 MB)   | SPIR-V↔LLVM |
| `ohos/llvm-spirv` (4.2 MB)              | tool |

## Reproduce / verify

```bash
# 0. clone the source, pinned to the exact commit
git clone --branch OpenHarmony-7.0-Release --single-branch \
    https://github.com/frankplus/third_party_mesa3d.git third_party/mesa3d-new
git -C third_party/mesa3d-new checkout 2d564becbaddf0ce464c66b8f6682aa8b80bd094

# 1. build + deploy into this lib64/ (runs in the build container, after a
#    product build so out/x86_general has zlib, expat, libqos, the sysroot...)
sudo docker exec -u root -w /home/openharmony/workdir/out/x86_general <container> \
    bash /home/openharmony/workdir/device/soc/oniro/x86_general/hardware/gpu/lib64/oniro_build_x86_general.sh

# 2. verify version of the deployed lib
strings libgallium_dri.so | grep -m1 'Mesa 2'         # -> Mesa 25.0.7
```

Prerequisites: `meson`/`mako`/`pyyaml` in the build container
(`pip3 install meson==1.7.0 mako pyyaml`). The `clang-dev/bin/llvm-config`
wrapper is created automatically by the build script.

Deployed `libgallium_dri.so` sha256:
`89eb32b18ff7dae979ab89d391a2a8755e9328841cd7e8a36ffd2561b3f09c1c`
(not build-reproducible bit-for-bit: paths/timestamps vary; the recipe is the
provenance, not a fixed hash.)
