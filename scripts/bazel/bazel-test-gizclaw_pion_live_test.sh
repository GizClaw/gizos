#!/bin/sh
set -eu

exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/cc_test/gizclaw:gizclaw_pion_live_test
