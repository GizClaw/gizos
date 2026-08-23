#!/bin/sh
set -eu

ipa=$1
test -s "$ipa"
artifact_dir=$(mktemp -d "${TMPDIR:-/tmp}/h2-ios-package.XXXXXX")
trap 'rm -rf "$artifact_dir"' EXIT HUP INT TERM
unzip -q "$ipa" -d "$artifact_dir"
app="$artifact_dir/Payload/FirmwaresTapReset.app"
test -x "$app/FirmwaresTapReset"
file "$app/FirmwaresTapReset" | grep -q 'Mach-O 64-bit executable arm64'
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Info.plist")" = com.haivivi.firmwares.smokeapps.tapreset
