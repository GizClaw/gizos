#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/android_binary/pal-webrtc:android_pal_webrtc_simulator_test \
  --config=android_arm64
