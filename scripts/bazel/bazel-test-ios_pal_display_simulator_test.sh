#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/testing/targets/ios_application/pal-display:ios_pal_display_simulator_test
