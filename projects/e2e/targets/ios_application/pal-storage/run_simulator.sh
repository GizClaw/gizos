#!/bin/sh
set -eu
exec python3 projects/e2e/libs/pal-storage-mobile/run_mobile.py "$@"
