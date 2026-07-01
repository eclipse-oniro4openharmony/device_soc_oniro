#!/bin/bash
# Reproducible cross-build + deploy of the Oniro x86_general Mesa GPU driver
# (libgallium_dri.so + libEGL/libGLESv*/libgbm) from source.
#
# Source: re-clone diemit's fork into third_party/mesa3d-new first (see
# lib64/PROVENANCE.md), then run this. It builds AND installs the matched lib
# set into this lib64/ directory (renaming libEGL.so -> libEGL.so.1.0.0 to
# match BUILD.gn).
#
# Provenance context
# -------------------
# The prebuilt device/soc/oniro/x86_general/hardware/gpu/lib64/libgallium_dri.so
# checked into the Oniro tree was built by community contributor "diemit"
# (598757652@qq.com) from Mesa 22.2.5, cross-compiled for OHOS x86_general
# (musl, OHOS clang 15.0.4) in his private ohos_60 / "opc" tree. That exact
# 22.2.5 source is not published. diemit's public fork
#   https://gitcode.com/diemit/third_party_mesa3d.git  (branch mesa25.0.1_x86_dev)
# carries Mesa 25.0.7 plus the OHOS-x86 build recipe used below
# (ohos/build_ohos86.py + ohos/meson_cross_process86.py). This script rebuilds
# from that public source so the driver has reproducible provenance.
#
# Deviations from diemit's ohos/build_ohos86.py, and why:
#  * gallium-drivers = swrast,llvmpipe,softpipe,virgl  (diemit: radeonsi,swrast,
#    llvmpipe,softpipe). radeonsi and vulkan-drivers=amd (radv) need an LLVM
#    built WITH the AMDGPU target; the OHOS prebuilt clang-15 llvm-config and
#    headers ship X86 but NOT AMDGPU, and diemit's matching private "clang-dev"
#    LLVM is unpublished. virgl (virtio_gpu) + llvmpipe/softpipe are what the
#    QEMU virtio-gpu emulator actually uses and are buildable from public bits.
#  * clang-dev/bin/llvm-config is a wrapper (created by step 0 below) that
#    reports LLVM 15.0.4 / X86 from the prebuilt toolchain but redirects link
#    info to the target static archives in ohos/llvm_libx86; the cross_file
#    link_args force the actual static linking.
#  * zlib.pc is repointed at the static out/.../obj/third_party/zlib/libz.a
#    (the 6.1 out/ layout has no plain libz.so; LLVMSupport needs crc32 etc.).
#  * -Dunversion-libgallium=true so the megadriver is named libgallium_dri.so
#    (matching lib64/BUILD.gn and the checked-in artifact), not
#    libgallium-25.0.7.so.
#
# Run INSIDE the build container (id 3127b8693e81), e.g.:
#   sudo docker exec -u root -w /home/openharmony/workdir/out/x86_general 3127b8693e81 \
#       bash /home/openharmony/workdir/device/soc/oniro/x86_general/hardware/gpu/lib64/oniro_build_x86_general.sh
set -euo pipefail

WORKDIR=/home/openharmony/workdir
PRODUCT=x86_general
SRC=$WORKDIR/third_party/mesa3d-new
BUILDROOT=$WORKDIR/out/$PRODUCT              # CWD convention from build_ohos86.py
BUILDDIR=thirdparty/mesa3d/build-ohos        # relative to BUILDROOT
PREFIX=$BUILDROOT/thirdparty/mesa3d

export PATH=$WORKDIR/prebuilts/build-tools/linux-x86/bin:/usr/local/bin:$PATH
cd "$BUILDROOT"

# 0. Create the llvm-config wrapper that meson uses (referenced by the cross_file
#    [binaries] section as $WORKDIR/clang-dev/bin/llvm-config). It reports LLVM
#    15.0.4 / X86 from the prebuilt OHOS toolchain so meson detects LLVM, but
#    redirects link info to the target static archives in mesa3d-new/ohos/
#    llvm_libx86 (the cross_file link_args force the actual static linking).
mkdir -p "$WORKDIR/clang-dev/bin"
cat > "$WORKDIR/clang-dev/bin/llvm-config" <<'WRAP'
#!/bin/bash
set -u
WORKDIR="/home/openharmony/workdir"
REAL="$WORKDIR/prebuilts/clang/ohos/linux-x86_64/llvm/bin/llvm-config"
LLVM_LIBX86="$WORKDIR/third_party/mesa3d-new/ohos/llvm_libx86"
PREBUILT_INC="$WORKDIR/prebuilts/clang/ohos/linux-x86_64/llvm/include"
contains() { local n="$1"; shift; for a in "$@"; do [ "$a" = "$n" ] && return 0; done; return 1; }
if   contains --libs "$@" || contains --libfiles "$@"; then echo ""            # libs via cross_file link_args
elif contains --system-libs "$@"; then echo ""                                 # zlib via pkg-config / cross_file
elif contains --ldflags "$@";     then echo "-L$LLVM_LIBX86"
elif contains --libdir "$@";      then echo "$LLVM_LIBX86"
elif contains --shared-mode "$@"; then echo "static"
elif contains --includedir "$@";  then echo "$PREBUILT_INC"
else exec "$REAL" "$@"
fi
WRAP
chmod +x "$WORKDIR/clang-dev/bin/llvm-config"

# 1. Generate the x86_64-ohos meson cross_file + pkg-config .pc set.
python3 "$SRC/ohos/meson_cross_process86.py" "$WORKDIR" "$PRODUCT" none none new_skia

# 2. Repoint zlib at the target static archive (6.1 out/ layout fix).
sed -i 's#^Libs:.*#Libs: ${libdir}/libz.a#' thirdparty/mesa3d/pkgconfig/zlib.pc

export PKG_CONFIG_PATH=$BUILDROOT/thirdparty/mesa3d/pkgconfig

# 3. Configure.
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
  --cross-file=thirdparty/mesa3d/cross_file \
  --prefix="$PREFIX"

# 4. Build + install.
ninja -C "$BUILDDIR" -j"$(nproc)"
ninja -C "$BUILDDIR" install

# 5. Deploy the matched set into lib64/ (libEGL.so -> libEGL.so.1.0.0 for BUILD.gn).
#    libglapi.so.0.0.0 is left as-is (the 25.0.7 set links glapi statically).
LIB64=$WORKDIR/device/soc/oniro/x86_general/hardware/gpu/lib64
install -m0755 "$PREFIX/lib/libgallium_dri.so"      "$LIB64/libgallium_dri.so"
install -m0755 "$PREFIX/lib/libEGL.so"              "$LIB64/libEGL.so.1.0.0"
install -m0755 "$PREFIX/lib/libGLESv2.so.2.0.0"     "$LIB64/libGLESv2.so.2.0.0"
install -m0755 "$PREFIX/lib/libGLESv1_CM.so.1.1.0"  "$LIB64/libGLESv1_CM.so.1.1.0"
install -m0755 "$PREFIX/lib/libgbm.so.1.0.0"        "$LIB64/libgbm.so.1.0.0"

echo
echo "Built artifacts in $PREFIX/lib :"; ls -l "$PREFIX/lib/"
echo "Deployed into $LIB64 :"; ls -l "$LIB64"/*.so*
