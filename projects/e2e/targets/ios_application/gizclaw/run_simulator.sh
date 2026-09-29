#!/bin/sh
set -eu
exec python3 projects/e2e/libs/gizclaw-mobile/run_mobile.py "$@" --output "${TEST_UNDECLARED_OUTPUTS_DIR:?}"
