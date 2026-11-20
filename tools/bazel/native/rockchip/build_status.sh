#!/bin/sh
# Upstream Make and packaging tools use the local Linux installation, including
# the static libc and compiler internals. Isolate all four actions per invocation.
# The stable-status file is an action input only. Its nonce never enters the image.
set -eu
read -r invocation < /proc/sys/kernel/random/uuid
printf 'STABLE_R36S_BUILD_INVOCATION %s\n' "$invocation"
