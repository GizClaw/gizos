#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/testing/targets/android_binary/pal-wifi:android_pal_wifi_simulator_test \
  --config=android_arm64
