# Oniro emulator_arm64 Mesa GPU driver — provenance & build

The prebuilt GPU libraries in this directory (`libgallium_dri.so`,
`libEGL.so.1.0.0`, `libGLESv2.so.2.0.0`, `libGLESv1_CM.so.1.1.0`,
`libgbm.so.1.0.0`) are **Mesa 25.0.7**, cross-compiled from source for
`aarch64-linux-ohosmusl`. They are built from the same Mesa source as the
x86_64 emulator's (see `../x86_64/PROVENANCE.md` for the fork and its
commits), with the same drivers: `llvmpipe`, `softpipe` and `virgl`.

## Source

```
github.com/frankplus/third_party_mesa3d
branch OpenHarmony-7.0-Release @ 82d2a611e7bffe5c8f8e5833246530df98e10221
```

The same commit as the x86_64 libraries; `../x86_64/PROVENANCE.md` lists
what the branch adds to upstream. The arm64 build uses
`ohos/meson_cross_process_oniro.py` (commit `253367c`), the cross file /
pkg-config generator that takes the target architecture and a directory of
static LLVM libraries.

## LLVM

llvmpipe JIT-compiles shaders with LLVM, which `libgallium_dri.so` links
statically. The x86_64 build uses diemit's prebuilt LLVM archives; nothing
equivalent exists for aarch64, and the tree's prebuilt clang ships no LLVM
target libraries (and its `llvm-config.h` names X86 as the native target). So
**`../build_llvm.sh`** builds upstream **LLVM 15.0.7** (the release line of the
tree's OHOS clang 15.0.4) for `aarch64-unknown-linux-ohos`:

* the tarballs from the llvm-project GitHub release, checked against pinned
  sha256 sums;
* the AArch64 backend only, static, PIC, Release, no tools, zlib, zstd,
  terminfo or libxml2;
* compiled with the tree's prebuilt clang against the product's musl sysroot.
  This matters: the tree's libc++ uses the ABI namespace `std::__h`, the
  OpenHarmony SDK's `std::__n1`, and an LLVM built with the SDK fails to link
  into Mesa.

## Build

```bash
# 0. the Mesa source, pinned
git clone --branch OpenHarmony-7.0-Release --single-branch \
    https://github.com/frankplus/third_party_mesa3d.git third_party/mesa3d-new
git -C third_party/mesa3d-new checkout 82d2a611e7bffe5c8f8e5833246530df98e10221

# 1. an emulator_arm64 product build (sysroot, zlib, expat, libdrm, hilog, qos)
#    so that out/emulator_arm64 exists. A first build before this directory has
#    real libraries needs placeholders (empty files under the five names);
#    deps_guard then fails at the end, which is expected.

# 2. LLVM, on the host (about 10 min on 46 cores)
bash device/soc/oniro/emulator/hardware/gpu/build_llvm.sh \
    --ohos-root . --prefix out/emulator_arm64-llvm

# 3. Mesa, in the build container; deploys into this directory
sudo docker exec -u root -w /home/openharmony/workdir/out/emulator_arm64 <container> \
    bash /home/openharmony/workdir/device/soc/oniro/emulator/hardware/gpu/arm64/oniro_build_arm64.sh

# 4. rebuild the product to package them
```

`oniro_build_arm64.sh` puts an `llvm-config` wrapper in front of meson: the
tree's prebuilt `llvm-config` answers version and component queries, and the
include and library locations point at the LLVM prefix. The meson options are
the x86_64 ones. The x86_64-only SPIRV-Tools archives in the fork are left
out; they are optional (SPIR-V validation in vtn) and CLC is off.

`GL_RENDERER` reports `llvmpipe (LLVM 15.0.4, ...)`: the version comes from
the tree's `llvm-config`, which the wrapper passes through; the code linked in
is 15.0.7.

Deployed `libgallium_dri.so` sha256:
`6f28256833b4d010ecb3d41922b455d41e3b8d9084269dcef1add8bc578966ca`
(not reproducible bit for bit: build paths vary. The recipe is the
provenance, not the hash.)
