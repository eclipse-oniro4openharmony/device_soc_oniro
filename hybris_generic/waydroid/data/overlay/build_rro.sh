#!/usr/bin/env bash
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
#
# build_rro.sh — rebuild the two resource overlays this product grafts into
# the container's /odm/overlay:
#
#   OniroNoBars.apk          Android draws no status bar and no navigation
#                            bar of its own (launcher plan L0 / LD5)
#   OniroInstallerOpaque.apk Android's install confirmation is an opaque
#                            activity, so the hwc gives its task a window and
#                            the user can answer it (see installer/)
#
#   build_rro.sh                     both target APKs come from the running
#                                    container on the connected device (hdc)
#   build_rro.sh <framework-res.apk> [<PackageInstaller.apk>]
#                                    ... or from files
#
# Both APKs are committed prebuilt; run this after bumping the image pins in
# ../images.manifest.  The build IS the compatibility check: aapt2 refuses to
# link an overlay for a resource the target does not define, so a renamed
# resource fails here instead of silently bringing the bars back — or, for the
# installer, leaving every store waiting on a dialog nobody can see.
#
# Needs: java (jarsigner + keytool), python3, curl, unzip — and hdc for the
# first form.
#
# Why these tools: aapt2 comes from Google's Maven (a static binary inside a
# jar), so no Android SDK is needed.  The target APKs come from the device
# because the system image is EROFS and hosts rarely have the tools to read
# one.  The v1 (JAR) signature is enough: the manifest declares no
# targetSdkVersion, and APKs scanned from a system partition (/odm/overlay)
# have their certs collected, not verified — the key has no security role and
# is thrown away.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AAPT2_V=8.5.0-11315950
FW_NAMES="dimen/status_bar_height_default dimen/status_bar_height_portrait
dimen/status_bar_height_landscape dimen/quick_qs_offset_height
bool/config_showNavigationBar"
PI_NAMES="style/Theme.AlertDialogActivity
style/TextAppearance.PackageInstaller.Title
attr/textAppearanceInstallerTitle"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

# Copy an APK out of the running container (its /system is EROFS, so it has
# to come through a shell there) into $2.
pull_from_container() {
    hdc shell "P=\$(cat /data/waydroid/container.pid 2>/dev/null); [ -n \"\$P\" ] &&
        cp /proc/\$P/root/$1 /data/local/tmp/wd-target.apk &&
        chmod 644 /data/local/tmp/wd-target.apk" >/dev/null
    hdc file recv /data/local/tmp/wd-target.apk "$2" >/dev/null
    hdc shell 'rm -f /data/local/tmp/wd-target.apk' >/dev/null
}

FW="${1:-}"
if [ -z "$FW" ]; then
    FW="$W/framework-res.apk"
    pull_from_container system/framework/framework-res.apk "$FW"
fi
[ -s "$FW" ] || { echo "no framework-res.apk (is the container running?)" >&2; exit 1; }

PI="${2:-}"
if [ -z "$PI" ]; then
    PI="$W/PackageInstaller.apk"
    pull_from_container system/priv-app/PackageInstaller/PackageInstaller.apk "$PI"
fi
[ -s "$PI" ] || { echo "no PackageInstaller.apk (is the container running?)" >&2; exit 1; }

curl -sfL -o "$W/aapt2.jar" \
  "https://dl.google.com/android/maven2/com/android/tools/build/aapt2/$AAPT2_V/aapt2-$AAPT2_V-linux.jar"
unzip -o -q "$W/aapt2.jar" aapt2 -d "$W" && chmod +x "$W/aapt2"

# $1: target APK, $2: what it is, $3...: resources the overlay needs it to have
check_names() {
    target="$1"; what="$2"; shift 2
    "$W/aapt2" dump resources "$target" 2>/dev/null >"$W/names.txt"
    for n in "$@"; do
        grep -q "resource 0x[0-9a-f]* $n\$" "$W/names.txt" ||
            { echo "$what no longer defines $n — the overlay needs rework" >&2; exit 1; }
    done
}
check_names "$FW" "framework-res.apk" $FW_NAMES
check_names "$PI" "PackageInstaller.apk" $PI_NAMES

keytool -genkeypair -keystore "$W/k.jks" -storepass throwaway -keypass throwaway -alias k \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Oniro Waydroid overlay" >/dev/null 2>&1

# $1: source directory, $2: output APK, $3...: what to link against
build_overlay() {
    src="$1"; out="$2"; shift 2
    inc=()
    for i in "$@"; do inc+=(-I "$i"); done
    rm -f "$W/res.zip" "$W/unsigned.apk" "$W/signed.apk"
    "$W/aapt2" compile --dir "$HERE/$src/res" -o "$W/res.zip"
    "$W/aapt2" link -o "$W/unsigned.apk" --manifest "$HERE/$src/AndroidManifest.xml" \
        "${inc[@]}" "$W/res.zip"
    cp "$W/unsigned.apk" "$W/signed.apk"
    jarsigner -keystore "$W/k.jks" -storepass throwaway -keypass throwaway \
        -sigalg SHA256withRSA -digestalg SHA-256 "$W/signed.apk" k >/dev/null
    # Aligning last is what makes the overlay installable-alongside: see
    # zipalign.py for why an unaligned resources.arsc here breaks every APK
    # install in the container, not just this APK.
    python3 "$HERE/zipalign.py" "$W/signed.apk" "$HERE/$out"
}

build_overlay nobars OniroNoBars.apk "$FW"
# The installer overlay reaches into its target's private resources, so that
# target has to be on the include path too.
build_overlay installer OniroInstallerOpaque.apk "$FW" "$PI"
ls -la "$HERE/OniroNoBars.apk" "$HERE/OniroInstallerOpaque.apk"
