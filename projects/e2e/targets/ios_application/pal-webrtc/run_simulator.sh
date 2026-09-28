#!/bin/sh
set -eu
exec python3 projects/e2e/libs/pal-webrtc-mobile/run_mobile.py "$@"
