#!/bin/sh
set -eu

apk=$1
media=$2
test -s "$apk"
: "${ANDROID_HOME:?ANDROID_HOME is required}"
: "${ANDROID_BUILD_TOOLS_VERSION:?ANDROID_BUILD_TOOLS_VERSION is required}"
build_tools="$ANDROID_HOME/build-tools/$ANDROID_BUILD_TOOLS_VERSION"
"$build_tools/apksigner" verify "$apk"
"$build_tools/aapt" dump badging "$apk" |
  grep -q "package: name='com.haivivi.firmwares.smokeapps.mp4player'"
"$build_tools/aapt" dump badging "$apk" |
  grep -q "launchable-activity: name='com.haivivi.firmwares.smokeapps.mp4player.MainActivity'"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/h2-android-mp4-player.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM
unzip -q "$apk" -d "$artifact_dir"
library="$artifact_dir/lib/arm64-v8a/libmp4_player_app.so"
test -s "$library"
file "$library" | grep -q 'ELF 64-bit.*ARM aarch64'
cmp "$media" "$artifact_dir/assets/test_1024x600_h264_aac.mp4"
test "$(find "$artifact_dir/assets" -type f | wc -l | tr -d ' ')" = 1
