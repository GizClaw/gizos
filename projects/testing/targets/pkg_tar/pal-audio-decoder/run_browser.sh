#!/bin/sh
set -eu
exec python3 projects/testing/targets/pkg_tar/pal-audio-decoder/run_browser.py "$@"
