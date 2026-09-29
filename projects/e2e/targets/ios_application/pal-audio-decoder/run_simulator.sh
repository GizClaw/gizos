#!/bin/sh
set -eu
exec python3 projects/e2e/libs/pal-audio-decoder-mobile/run_mobile.py "$@"
