#!/bin/sh
set -eu
exec python3 projects/e2e/libs/pal-audio-mobile/run_mobile.py "$@"
