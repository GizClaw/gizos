#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/testing/targets/android_binary/pal-core:android_pal_core_simulator_test \
  --config=android_arm64
