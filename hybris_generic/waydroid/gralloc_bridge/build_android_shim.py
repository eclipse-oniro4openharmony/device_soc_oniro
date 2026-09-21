#!/usr/bin/env python3
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
"""Assemble + link binderflags_shim.S as an *Android* shared object.

The shim is loaded (LD_PRELOAD) by a process inside the Android container, so
it must be an aarch64-linux-android object with no OHOS libc dependency — the
OHOS toolchain targets in GN can not produce that, hence this action.  The
source is position-independent assembly with one weak import, so no sysroot or
libc is needed for either step.
"""
import argparse
import os
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--clang-dir', required=True)
    ap.add_argument('--source', required=True)
    ap.add_argument('--soname', required=True)
    ap.add_argument('--output', required=True)
    args = ap.parse_args()

    obj = args.output + '.o'
    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    subprocess.check_call([
        os.path.join(args.clang_dir, 'bin', 'clang'),
        '--target=aarch64-linux-android', '-c', args.source, '-o', obj])
    subprocess.check_call([
        os.path.join(args.clang_dir, 'bin', 'ld.lld'),
        '-shared', '-soname', args.soname, '--allow-shlib-undefined',
        '--build-id=none', '-o', args.output, obj])
    os.remove(obj)
    return 0


if __name__ == '__main__':
    sys.exit(main())
