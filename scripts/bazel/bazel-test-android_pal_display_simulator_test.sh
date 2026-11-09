#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/testing/targets/android_binary/pal-display:android_pal_display_simulator_test
