#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/cc_binary/pal-net-tls:desktop_pal_net_tls_endpoint_rejection_test
