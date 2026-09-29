#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/pkg_tar/pal-net-tls:wasm_pal_net_tls_boundary_test
