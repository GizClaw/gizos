#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/android_binary/pal-storage:android_pal_storage_simulator_test \
  --config=android_arm64
