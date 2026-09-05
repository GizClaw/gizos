#!/bin/sh
set -eu

: "${H2_GIZCLAW_E2E_ENDPOINT:?set H2_GIZCLAW_E2E_ENDPOINT to hostname:port}"

exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/cc_test/gizclaw:gizclaw_pion_live_test \
  "--test_arg=--endpoint=$H2_GIZCLAW_E2E_ENDPOINT"
