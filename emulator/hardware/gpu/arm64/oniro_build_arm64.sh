#!/bin/bash
#
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Cross-build + deploy the arm64 emulator's Mesa (libgallium_dri.so with
# llvmpipe/softpipe/virgl, libEGL/libGLESv*/libgbm) from source, into this
# directory. See PROVENANCE.md next to it.
#
# Prerequisites:
#  1. The Mesa source at third_party/mesa3d-new (PROVENANCE.md has the pin).
#  2. A static LLVM for aarch64-linux-ohos, built on the host by
#       device/soc/oniro/emulator/hardware/gpu/build_llvm.sh \
#         --ohos-root . --prefix out/emulator_arm64-llvm
#  3. An emulator_arm64 product build in out/emulator_arm64 (sysroot, zlib,
#     expat, libdrm, hilog, qos, ...): Mesa links against them.
#  4. meson >= 1.7, mako and pyyaml in the build container.
#
# Run INSIDE the build container, e.g.:
#   sudo docker exec -u root -w /home/openharmony/workdir/out/emulator_arm64 \
#       <container> bash /home/openharmony/workdir/device/soc/oniro/emulator/hardware/gpu/arm64/oniro_build_arm64.sh
#
# LLVM_PREFIX overrides the LLVM location (default out/emulator_arm64-llvm).
set -euo pipefail

WORKDIR=/home/openharmony/workdir
PRODUCT=emulator_arm64
SRC=$WORKDIR/third_party/mesa3d-new
BUILDROOT=$WORKDIR/out/$PRODUCT
BUILDDIR=thirdparty/mesa3d/build-ohos        # relative to BUILDROOT
PREFIX=$BUILDROOT/thirdparty/mesa3d
LLVM_PREFIX=${LLVM_PREFIX:-$WORKDIR/out/emulator_arm64-llvm}
DEST=$WORKDIR/device/soc/oniro/emulator/hardware/gpu/arm64

export PATH=$WORKDIR/prebuilts/build-tools/linux-x86/bin:/usr/local/bin:$PATH
cd "$BUILDROOT"

[ -f "$LLVM_PREFIX/include/llvm/Config/llvm-config.h" ] ||
  { echo "no LLVM in $LLVM_PREFIX (run build_llvm.sh first)" >&2; exit 1; }
grep -q 'LLVM_NATIVE_ARCH AArch64' "$LLVM_PREFIX/include/llvm/Config/llvm-config.h" ||
  { echo "$LLVM_PREFIX is not an AArch64 LLVM" >&2; exit 1; }

# 0. llvm-config for meson. The prebuilt OHOS llvm-config (LLVM 15.0.4, the
#    same release line as the LLVM 15.0.7 libraries) answers the version and
#    component queries; include and library locations come from LLVM_PREFIX,
#    whose generated llvm-config.h names AArch64 as the native target. The
#    libraries themselves are linked through the cross file.
WRAP_DIR=$BUILDROOT/thirdparty/mesa3d/llvm-config-wrapper
mkdir -p "$WRAP_DIR"
cat > "$WRAP_DIR/llvm-config" <<WRAP
#!/bin/bash
set -u
REAL="$WORKDIR/prebuilts/clang/ohos/linux-x86_64/llvm/bin/llvm-config"
REAL_INC="$WORKDIR/prebuilts/clang/ohos/linux-x86_64/llvm/include"
LLVM_PREFIX="$LLVM_PREFIX"
contains() { local n="\$1"; shift; for a in "\$@"; do [ "\$a" = "\$n" ] && return 0; done; return 1; }
if   contains --libs "\$@" || contains --libfiles "\$@"; then echo ""
elif contains --system-libs "\$@"; then echo ""
elif contains --ldflags "\$@";     then echo "-L\$LLVM_PREFIX/lib"
elif contains --libdir "\$@";      then echo "\$LLVM_PREFIX/lib"
elif contains --shared-mode "\$@"; then echo "static"
elif contains --includedir "\$@";  then echo "\$LLVM_PREFIX/include"
elif contains --targets-built "\$@"; then echo "AArch64"
elif contains --host-target "\$@"; then echo "aarch64-unknown-linux-ohos"
elif contains --cppflags "\$@" || contains --cflags "\$@" || contains --cxxflags "\$@"; then
  "\$REAL" "\$@" | sed "s#\$REAL_INC#\$LLVM_PREFIX/include#g"
else exec "\$REAL" "\$@"
fi
WRAP
chmod +x "$WRAP_DIR/llvm-config"

# 1. Cross file + pkg-config set for aarch64-linux-ohos.
python3 "$SRC/ohos/meson_cross_process_oniro.py" "$WORKDIR" "$PRODUCT" arm64 \
  "$LLVM_PREFIX/lib" "$WRAP_DIR/llvm-config"

# 2. zlib and expat: link the product's static archives.
sed -i 's#^Libs:.*#Libs: ${libdir}/libz.a#' thirdparty/mesa3d/pkgconfig/zlib.pc
[ -f obj/third_party/expat/libexpat_static.a ] || ninja -w dupbuild=warn expat_static
sed -i "s#^Libs:.*#Libs: $BUILDROOT/obj/third_party/expat/libexpat_static.a#" thirdparty/mesa3d/pkgconfig/expat.pc

export PKG_CONFIG_PATH=$BUILDROOT/thirdparty/mesa3d/pkgconfig

# 3. Configure: the same options as the x86_64 build.
rm -rf "$BUILDDIR"
meson setup "$SRC" "$BUILDDIR" \
  -Dplatforms=ohos -Degl-native-platform=ohos -Dgbm=enabled \
  -Dgallium-drivers=swrast,llvmpipe,softpipe,virgl \
  -Dvulkan-drivers= \
  -Dbuildtype=release \
  -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true -Dcpp_rtti=false \
  -Dglx=disabled -Dtools= -Dglvnd=disabled -Dgallium-va=disabled \
  -Dshared-glapi=enabled -Dshader-cache=disabled -Dzlib=enabled \
  -Dunversion-libgallium=true \
  -Ddri-drivers-path=/vendor/lib64/chipsetsdk \
  -Dllvm=enabled -Dshared-llvm=disabled \
  -Dlibunwind=disabled \
  --cross-file=thirdparty/mesa3d/cross_file \
  --prefix="$PREFIX"

# 4. Build + install.
ninja -C "$BUILDDIR" -j"$(nproc)"
ninja -C "$BUILDDIR" install

# 5. Deploy under the names mesa/BUILD.gn expects.
install -m0755 "$PREFIX/lib/libgallium_dri.so"      "$DEST/libgallium_dri.so"
install -m0755 "$PREFIX/lib/libEGL.so"              "$DEST/libEGL.so.1.0.0"
install -m0755 "$PREFIX/lib/libGLESv2.so.2.0.0"     "$DEST/libGLESv2.so.2.0.0"
install -m0755 "$PREFIX/lib/libGLESv1_CM.so.1.1.0"  "$DEST/libGLESv1_CM.so.1.1.0"
install -m0755 "$PREFIX/lib/libgbm.so.1.0.0"        "$DEST/libgbm.so.1.0.0"

echo
echo "Deployed into $DEST :"; ls -l "$DEST"/*.so*
