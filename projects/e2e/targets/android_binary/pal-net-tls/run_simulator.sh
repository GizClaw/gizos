#!/bin/sh
set -eu
exec python3 projects/e2e/libs/pal-net-tls-mobile/run_mobile.py "$@"
