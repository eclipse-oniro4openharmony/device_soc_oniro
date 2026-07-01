# Oniro x86_general Mesa GPU driver — provenance & build

The prebuilt GPU libraries in this directory (`libgallium_dri.so`,
`libEGL.so.1.0.0`, `libGLESv2.so.2.0.0`, `libGLESv1_CM.so.1.1.0`,
`libgbm.so.1.0.0`) are **Mesa 25.0.7**, cross-compiled from source for
`x86_64-linux-ohosmusl`. This file records where they come from and how to
rebuild them. `libglapi.so.0.0.0` is the older Mesa 22.2.5 lib kept as-is (the
25.0.7 set links glapi statically and does not need it).

> History: before this rebuild, `libgallium_dri.so` was an opaque **Mesa
> 22.2.5** blob built by contributor *diemit* in his private tree — its RUNPATH
> still baked in `/home/diemit/ohos_60/...`. It was replaced by this from-source
> build; the originals are in `../lib64.orig-22.2.5/`.

## Provenance chain

```
freedesktop.org Mesa 25.0.7                       (canonical upstream)
  └─> OpenHarmony mesa fork  (OHOS platform port + ohos/ build glue)
        github.com/eclipse-oniro-mirrors/third_party_mesa3d   (in-tree: third_party/mesa3d, 25.0.1)
      └─> diemit fork         (adds x86_64 cross-build, drivers, radv)
            gitcode.com/diemit/third_party_mesa3d.git
            branch mesa25.0.1_x86_dev @ 135d43ab0016c1b0c93d08a7a91a9fd892e86455  "adpt radv"
          └─> these binaries   (built from that source; see build recipe below)
```

The source is vendored as a **pinned git checkout** at **`third_party/mesa3d-new`**
— HEAD = `135d43ab` ("adpt radv"), remotes `diemit` (gitcode) + `openharmony`
(eclipse-oniro-mirrors). The build reads directly from that tree; there are no
patches. `git -C third_party/mesa3d-new status` is clean — the source is
byte-identical to diemit @`135d43ab` (our build recipe lives here in `lib64/`,
not inside the mesa tree).

To see what diemit changed vs OpenHarmony upstream (the in-tree
`third_party/mesa3d`, Mesa 25.0.1), diff the trees — diemit **only added** x86
support (the arm64 scripts are byte-identical to upstream):

```bash
# files diemit added (his OHOS/x86 port: build_ohos86.py, meson_cross_process86.py,
#   pkgconfig templates, radv_ohos.*, vk_ohos.c, llvm_libx86/, SPIRV-*, ...)
comm -13 <(cd third_party/mesa3d/ohos     && find . -type f | sort) \
         <(cd third_party/mesa3d-new/ohos && find . -maxdepth 1 | sort)
git -C third_party/mesa3d-new log --oneline   # diemit's 4 squashed commits
```

## How these binaries were built

Full recipe: **`oniro_build_x86_general.sh`** (in this directory). It runs inside
the build container (`3127b8693e81`) and:
1. generates the `x86_64-linux-ohosmusl` meson cross-file + pkg-config `.pc`
   set (diemit's `ohos/meson_cross_process86.py`), repointing `zlib.pc` at the
   static `out/.../obj/third_party/zlib/libz.a`;
2. `meson setup` with `-Dgallium-drivers=swrast,llvmpipe,softpipe,virgl
   -Dvulkan-drivers= -Dllvm=enabled -Dshared-llvm=disabled
   -Dunversion-libgallium=true` (→ output named `libgallium_dri.so`);
3. `ninja && ninja install`.

LLVM is provided by the tree's OHOS clang-15 (X86 target) plus diemit's prebuilt
static libs; meson sees it via the wrapper at `<tree>/clang-dev/bin/llvm-config`.

### Deviation from diemit's stock recipe
diemit's `ohos/build_ohos86.py` also builds `radeonsi` + `radv` (AMD). Those need
an LLVM built with the **AMDGPU** target; the OHOS prebuilt clang-15 ships X86
only and diemit's matching private LLVM is unpublished, so they are dropped.
`virgl` (virtio-gpu) + `llvmpipe`/`softpipe` are what the QEMU emulator uses and
are what these binaries contain.

### Binary prebuilts used at link time (from diemit @135d43ab, git-LFS)
| Path in `third_party/mesa3d-new/`      | Contents |
|----------------------------------------|----------|
| `ohos/llvm_libx86/` (86 `.a`, 219 MB)  | static LLVM-15 target libs (X86 used; AMDGPU present, unused here) |
| `ohos/SPIRV-Tools/` (3 `.a`, 18 MB)    | SPIR-V tools (unused by this driver set) |
| `ohos/SPIRV-LLVM-Translator/` (13 MB)  | SPIR-V↔LLVM (unused here) |
| `ohos/llvm-spirv` (4.2 MB)             | tool (unused here) |

These are LFS objects; their sha256 is recorded in diemit's tree at
`135d43ab`, so that commit is the authoritative provenance for them.

## Reproduce / verify

```bash
# 0. (re)clone the source — diemit's fork, pinned to the exact commit.
#    git-lfs must be installed; it smudges the prebuilt LLVM/SPIRV libs.
git lfs install
git clone --branch mesa25.0.1_x86_dev --single-branch \
    https://gitcode.com/diemit/third_party_mesa3d.git third_party/mesa3d-new
git -C third_party/mesa3d-new checkout 135d43ab0016c1b0c93d08a7a91a9fd892e86455
git -C third_party/mesa3d-new remote add openharmony \
    https://github.com/eclipse-oniro-mirrors/third_party_mesa3d   # optional, for diffing

# 1. build + deploy into this lib64/ (runs in the build container)
sudo docker exec -u root -w /home/openharmony/workdir/out/x86_general 3127b8693e81 \
    bash /home/openharmony/workdir/device/soc/oniro/x86_general/hardware/gpu/lib64/oniro_build_x86_general.sh

# 2. verify version of the deployed lib
strings libgallium_dri.so | grep -m1 'Mesa 2'         # -> Mesa 25.0.7
```

Prerequisites: `meson`/`mako`/`pyyaml` in the build container
(`pip3 install meson==1.7.0 mako pyyaml`). The `clang-dev/bin/llvm-config`
wrapper is created automatically by the build script.

Deployed `libgallium_dri.so` sha256:
`0de47b1f89e8ce4327fadbbb66f8681c26b15a7bd9229f3bfb2ceb2ba909ffdb`
(not build-reproducible bit-for-bit: paths/timestamps vary; the recipe is the
provenance, not a fixed hash.)
