#!/bin/sh
set -eu
exec python3 projects/testing/unit/pal-wifi/libs/mobile/run_mobile.py "$@"
