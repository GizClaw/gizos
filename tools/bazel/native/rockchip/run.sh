#!/bin/bash
# Each native action gets a private on-disk work directory beside its first output.
set -euo pipefail
umask 022
script=$(realpath "$1")
shift
paths=()
for path do paths+=("$(realpath -m "$path")"); done
scratch=$(mktemp -d "$(dirname "${paths[0]}")/.rk3326-XXXXXXXX")
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
jobs=$(nproc)
if (( jobs > 8 )); then jobs=8; fi
export KBUILD_BUILD_USER=gizos KBUILD_BUILD_HOST=builder KBUILD_BUILD_VERSION=1
export KBUILD_BUILD_TIMESTAMP="$(date -u -d "@$SOURCE_DATE_EPOCH" '+%a %b %d %T UTC %Y')"
source "$script" "${paths[@]}"
