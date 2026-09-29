#!/bin/sh
set -eu
exec python3 projects/e2e/targets/pkg_tar/pal-audio-decoder/run_browser.py "$@"
