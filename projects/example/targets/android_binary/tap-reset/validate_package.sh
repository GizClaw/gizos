#!/bin/sh
set -eu

apk=$1
test -s "$apk"
: "${ANDROID_HOME:?ANDROID_HOME is required}"
: "${ANDROID_BUILD_TOOLS_VERSION:?ANDROID_BUILD_TOOLS_VERSION is required}"
build_tools="$ANDROID_HOME/build-tools/$ANDROID_BUILD_TOOLS_VERSION"
"$build_tools/apksigner" verify "$apk"
"$build_tools/aapt" dump badging "$apk" |
  grep -q "package: name='com.haivivi.firmwares.smokeapps.tapreset'"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/h2-android-tap-reset.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM
unzip -q "$apk" -d "$artifact_dir"
library="$artifact_dir/lib/arm64-v8a/libtap_reset_app.so"
test -s "$library"
file "$library" | grep -q 'ELF 64-bit.*ARM aarch64'
