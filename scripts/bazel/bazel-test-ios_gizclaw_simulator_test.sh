#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/ios_application/gizclaw:ios_gizclaw_simulator_test \
  --config=ios_sim_arm64
