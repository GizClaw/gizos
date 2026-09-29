#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/android_binary/pal-net-tls:android_pal_net_tls_simulator_test \
  --config=android_arm64
