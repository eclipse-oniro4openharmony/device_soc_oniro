#!/usr/bin/env python3
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
#
# zipalign.py — 4-byte-align the stored entries of a zip, a pure-python
# stand-in for the Android SDK's zipalign (which build_rro.sh would otherwise
# have to drag in a whole SDK for).
#
# Why the overlay needs it: Android maps resources.arsc straight out of the
# APK, and only when the entry is stored AND its data starts on a 4-byte
# boundary; otherwise it copies the table into RAM and remembers that it did.
# A static RRO in /odm/overlay is loaded into *every* process's system assets,
# so one unaligned overlay makes AssetManager.containsAllocatedTable() true
# everywhere — and package parsing rejects any package targeting R+ with
# INSTALL_PARSE_FAILED_RESOURCES_ARSC_COMPRESSED (-124).  The symptom is that
# no APK can be installed in the container at all, however well-formed: "the
# resources.arsc of installed APKs [must] be stored uncompressed and aligned
# on a 4-byte boundary", about an APK that is both.
#
# Running after jarsigner is deliberate: a v1 (JAR) signature covers entry
# contents, which this preserves byte for byte, not zip offsets, which it
# moves.
import struct
import sys
import zipfile

LFH = 0x04034B50   # local file header
CDH = 0x02014B50   # central directory header
EOCD = 0x06054B50  # end of central directory


def dos_time(info):
    y, mo, d, h, mi, s = info.date_time
    return ((h << 11) | (mi << 5) | (s // 2),
            ((y - 1980) << 9) | (mo << 5) | d)


def align(src, dst, alignment=4):
    with open(src, "rb") as f:
        raw = f.read()
    out = bytearray()
    central = []
    for info in zipfile.ZipFile(src).infolist():
        off = info.header_offset
        if struct.unpack_from("<I", raw, off)[0] != LFH:
            raise ValueError(f"{info.filename}: bad local header")
        nlen, elen = struct.unpack_from("<HH", raw, off + 26)
        name = raw[off + 30:off + 30 + nlen]
        extra = raw[off + 30 + nlen:off + 30 + nlen + elen]
        data_off = off + 30 + nlen + elen
        data = raw[data_off:data_off + info.compress_size]

        # Only stored entries can be mapped in place, so only they need it.
        pad = 0
        if info.compress_type == zipfile.ZIP_STORED:
            pad = -(len(out) + 30 + nlen + len(extra)) % alignment
        central.append((info, name, len(out)))
        # Sizes and the CRC come from the central directory, so bit 3 (sizes
        # follow in a data descriptor) can be cleared: they are all here.
        out += struct.pack("<IHHHHHIIIHH", LFH, info.extract_version,
                           info.flag_bits & ~0x8, info.compress_type,
                           *dos_time(info), info.CRC, info.compress_size,
                           info.file_size, nlen, len(extra) + pad)
        out += name + extra + b"\0" * pad + data

    cd_off = len(out)
    for info, name, new_off in central:
        cextra = info.extra or b""
        comment = info.comment or b""
        out += struct.pack("<IHHHHHHIIIHHHHHII", CDH,
                           info.create_version | (info.create_system << 8),
                           info.extract_version, info.flag_bits & ~0x8,
                           info.compress_type, *dos_time(info), info.CRC,
                           info.compress_size, info.file_size, len(name),
                           len(cextra), len(comment), 0, info.internal_attr,
                           info.external_attr, new_off)
        out += name + cextra + comment
    out += struct.pack("<IHHHHIIH", EOCD, 0, 0, len(central), len(central),
                       len(out) - cd_off, cd_off, 0)
    with open(dst, "wb") as f:
        f.write(bytes(out))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: zipalign.py <in.apk> <out.apk>")
    align(sys.argv[1], sys.argv[2])
