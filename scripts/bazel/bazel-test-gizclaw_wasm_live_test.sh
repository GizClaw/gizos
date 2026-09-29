#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/pkg_tar/gizclaw:gizclaw_wasm_live_test
