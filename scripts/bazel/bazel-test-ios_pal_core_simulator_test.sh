#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/testing/targets/ios_application/pal-core:ios_pal_core_simulator_test \
  --config=ios_sim_arm64
