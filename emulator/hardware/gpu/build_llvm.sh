#!/bin/bash
#
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Cross-build the static LLVM that the arm64 emulator's Mesa (llvmpipe) links.
#
# Mesa's llvmpipe JIT-compiles shaders with LLVM, and libgallium_dri.so links
# LLVM statically. The OHOS prebuilt clang ships the LLVM headers but no target
# libraries, and its llvm-config.h names X86 as the native target, so neither
# fits an aarch64 Mesa. This script builds upstream LLVM 15.0.7 (the release
# line of the OHOS clang 15.0.4) for aarch64-linux-ohos with only the AArch64
# backend, as position-independent static libraries, and installs the headers
# and libraries into one prefix that arm64/oniro_build_arm64.sh points meson at.
#
# The x86_64 Mesa still links the LLVM libraries diemit published in the Mesa
# fork (ohos/llvm_libx86, see x86_64/PROVENANCE.md).
#
# Runs on the host, with the tree's own prebuilt clang and the product's musl
# sysroot, the toolchain Mesa and the rest of the image are built with. That
# matters: the tree's libc++ uses the ABI namespace std::__h, the SDK's
# std::__n1, and LLVM must link against the former. A host C++ compiler builds
# the native tablegen tools.
#
# Usage: build_llvm.sh --ohos-root DIR --prefix DIR [--product NAME] [--work DIR] [--jobs N]
#   --ohos-root   the OpenHarmony source tree
#   --product     product whose out/ has the sysroot (default emulator_arm64;
#                 out/<product>/obj/third_party/musl must exist, i.e. built)
#   --prefix      install prefix (include/, lib/libLLVM*.a)
#   --work        download + build directory (default: <prefix>.work)

set -euo pipefail

LLVM_VERSION=15.0.7
LLVM_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}
LLVM_SRC_SHA256=4ad8b2cc8003c86d0078d15d987d84e3a739f24aae9033865c027abae93ee7a4
CMAKE_SRC_SHA256=8986f29b634fdaa9862eedda78513969fe9788301c9f2d938f4c10a3e7a3e7ea

ohos_root=""
product=emulator_arm64
prefix=""
work=""
jobs=$(nproc)
while [[ $# -gt 0 ]]; do
  case "$1" in
    --ohos-root)  ohos_root="$2"; shift 2 ;;
    --product)    product="$2"; shift 2 ;;
    --prefix)     prefix="$2"; shift 2 ;;
    --work)       work="$2"; shift 2 ;;
    --jobs)       jobs="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$ohos_root" && -n "$prefix" ]] || {
  sed -n '/^# Usage/,/^$/p' "$0" >&2
  exit 2
}
ohos_root=$(cd "$ohos_root" && pwd)
clang_dir="$ohos_root/prebuilts/clang/ohos/linux-x86_64/llvm"
sysroot="$ohos_root/out/$product/obj/third_party/musl"
[[ -d "$sysroot/usr/lib/aarch64-linux-ohos" ]] || {
  echo "no aarch64 sysroot in $sysroot (build the $product product first)" >&2
  exit 1
}
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
work=${work:-$prefix.work}
mkdir -p "$work"
work=$(cd "$work" && pwd)

cmake_bin="$ohos_root/prebuilts/cmake/linux-x86/bin/cmake"
[[ -x "$cmake_bin" ]] || cmake_bin=cmake
ninja_bin="$ohos_root/prebuilts/build-tools/linux-x86/bin/ninja"
[[ -x "$ninja_bin" ]] || ninja_bin=ninja

fetch() {  # fetch <file> <sha256>
  local f="$work/$1"
  if [[ ! -f "$f" ]] || ! echo "$2  $f" | sha256sum -c --status; then
    curl -fL --retry 5 -o "$f.part" "$LLVM_URL/$1"
    mv "$f.part" "$f"
  fi
  echo "$2  $f" | sha256sum -c -
}
fetch "llvm-${LLVM_VERSION}.src.tar.xz" "$LLVM_SRC_SHA256"
fetch "cmake-${LLVM_VERSION}.src.tar.xz" "$CMAKE_SRC_SHA256"

src="$work/src"
if [[ ! -f "$src/llvm/CMakeLists.txt" ]]; then
  rm -rf "$src"
  mkdir -p "$src"
  tar -C "$src" -xf "$work/llvm-${LLVM_VERSION}.src.tar.xz"
  tar -C "$src" -xf "$work/cmake-${LLVM_VERSION}.src.tar.xz"
  mv "$src/llvm-${LLVM_VERSION}.src" "$src/llvm"
  mv "$src/cmake-${LLVM_VERSION}.src" "$src/cmake"
fi

# LLVM_TABLEGEN etc. are built for the host first (CROSS_TOOLCHAIN_FLAGS_NATIVE),
# then the libraries for the target. Only the libraries and headers are kept:
# no tools, tests, terminfo, zlib or libxml2 dependencies for the guest.
# The same target flags as Mesa's cross file (meson_cross_process_oniro.py).
toolchain="$work/aarch64-linux-ohos.cmake"
cat > "$toolchain" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT "$sysroot")
set(CMAKE_C_COMPILER "$clang_dir/bin/clang")
set(CMAKE_CXX_COMPILER "$clang_dir/bin/clang++")
set(CMAKE_C_COMPILER_TARGET aarch64-linux-ohos)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-ohos)
set(CMAKE_AR "$clang_dir/bin/llvm-ar")
set(CMAKE_RANLIB "$clang_dir/bin/llvm-ranlib")
set(CMAKE_C_FLAGS_INIT "-fno-emulated-tls -fPIC")
set(CMAKE_CXX_FLAGS_INIT "-fno-emulated-tls -fPIC")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld --rtlib=compiler-rt -L$sysroot/usr/lib/aarch64-linux-ohos -L$clang_dir/lib/aarch64-linux-ohos")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "\${CMAKE_EXE_LINKER_FLAGS_INIT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

build="$work/build-aarch64"
rm -rf "$build"
"$cmake_bin" -G Ninja -S "$src/llvm" -B "$build" \
  -DCMAKE_MAKE_PROGRAM="$ninja_bin" \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DLLVM_ENABLE_PIC=ON \
  -DLLVM_TARGETS_TO_BUILD=AArch64 \
  -DLLVM_TARGET_ARCH=AArch64 \
  -DLLVM_HOST_TRIPLE=aarch64-unknown-linux-ohos \
  -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-unknown-linux-ohos \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_BUILD_UTILS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_BINDINGS=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF \
  -DLLVM_BUILD_LLVM_DYLIB=OFF \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  "-DCROSS_TOOLCHAIN_FLAGS_NATIVE=-DCMAKE_C_COMPILER=${HOST_CC:-cc};-DCMAKE_CXX_COMPILER=${HOST_CXX:-c++};-DLLVM_INCLUDE_BENCHMARKS=OFF;-DLLVM_INCLUDE_TESTS=OFF;-DLLVM_INCLUDE_EXAMPLES=OFF;-DLLVM_ENABLE_ZLIB=OFF;-DLLVM_ENABLE_ZSTD=OFF;-DLLVM_ENABLE_TERMINFO=OFF;-DLLVM_ENABLE_LIBXML2=OFF"

"$ninja_bin" -C "$build" -j"$jobs" install-llvm-headers install-llvm-libraries
# llvm-config.h and the target .def files are generated; install-llvm-headers
# copies them too. Record what was built.
cat > "$prefix/BUILD_INFO" <<EOF
llvm ${LLVM_VERSION} (${LLVM_URL})
llvm src sha256 ${LLVM_SRC_SHA256}
target aarch64-unknown-linux-ohos, backend AArch64, static PIC, Release
compiler $("$clang_dir/bin/clang" --version | head -1), sysroot out/$product
EOF
echo "LLVM ${LLVM_VERSION} for aarch64-linux-ohos installed in $prefix"
ls "$prefix/lib" | head
