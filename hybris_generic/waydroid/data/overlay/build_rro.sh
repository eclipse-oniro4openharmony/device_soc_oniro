#!/usr/bin/env bash
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
#
# build_rro.sh — rebuild OniroNoBars.apk, the static resource overlay that
# removes Android's status bar and navigation bar (launcher plan L0 / LD5).
#
#   build_rro.sh                      framework-res.apk from the running
#                                     container on the connected device (hdc)
#   build_rro.sh <framework-res.apk>  ... or from a file
#
# The APK is committed prebuilt; run this after bumping the image pins in
# ../images.manifest.  The build IS the compatibility check: aapt2 refuses to
# link an overlay for a resource the target framework does not define, so a
# renamed resource fails here instead of silently bringing the bars back.
#
# Needs: java (jarsigner + keytool), python3, curl, unzip — and hdc for the
# first form.
#
# Why these tools: aapt2 comes from Google's Maven (a static binary inside a
# jar), so no Android SDK is needed.  framework-res.apk comes from the device
# because the system image is EROFS and hosts rarely have the tools to read
# one.  The v1 (JAR) signature is enough: the manifest declares no
# targetSdkVersion, and APKs scanned from a system partition (/odm/overlay)
# have their certs collected, not verified — the key has no security role and
# is thrown away.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AAPT2_V=8.5.0-11315950
NAMES="dimen/status_bar_height_default dimen/status_bar_height_portrait
dimen/status_bar_height_landscape dimen/quick_qs_offset_height
bool/config_showNavigationBar"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

FW="${1:-}"
if [ -z "$FW" ]; then
    FW="$W/framework-res.apk"
    hdc shell 'P=$(cat /data/waydroid/container.pid 2>/dev/null); [ -n "$P" ] &&
        cp /proc/$P/root/system/framework/framework-res.apk /data/local/tmp/wd-fwres.apk &&
        chmod 644 /data/local/tmp/wd-fwres.apk' >/dev/null
    hdc file recv /data/local/tmp/wd-fwres.apk "$FW" >/dev/null
    hdc shell 'rm -f /data/local/tmp/wd-fwres.apk' >/dev/null
fi
[ -s "$FW" ] || { echo "no framework-res.apk (is the container running?)" >&2; exit 1; }

curl -sfL -o "$W/aapt2.jar" \
  "https://dl.google.com/android/maven2/com/android/tools/build/aapt2/$AAPT2_V/aapt2-$AAPT2_V-linux.jar"
unzip -o -q "$W/aapt2.jar" aapt2 -d "$W" && chmod +x "$W/aapt2"

"$W/aapt2" dump resources "$FW" 2>/dev/null >"$W/names.txt"
for n in $NAMES; do
    grep -q "resource 0x[0-9a-f]* $n\$" "$W/names.txt" ||
        { echo "framework-res.apk no longer defines $n — the overlay needs rework" >&2; exit 1; }
done

"$W/aapt2" compile --dir "$HERE/nobars/res" -o "$W/res.zip"
"$W/aapt2" link -o "$W/unsigned.apk" --manifest "$HERE/nobars/AndroidManifest.xml" \
    -I "$FW" "$W/res.zip"
keytool -genkeypair -keystore "$W/k.jks" -storepass throwaway -keypass throwaway -alias k \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Oniro Waydroid overlay" >/dev/null 2>&1
cp "$W/unsigned.apk" "$W/signed.apk"
jarsigner -keystore "$W/k.jks" -storepass throwaway -keypass throwaway \
    -sigalg SHA256withRSA -digestalg SHA-256 "$W/signed.apk" k >/dev/null
# Aligning last is what makes the overlay installable-alongside: see zipalign.py
# for why an unaligned resources.arsc here breaks every APK install in the
# container, not just this APK.
python3 "$HERE/zipalign.py" "$W/signed.apk" "$HERE/OniroNoBars.apk"
ls -la "$HERE/OniroNoBars.apk"
