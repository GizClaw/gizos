#!/bin/sh
set -eu

exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/cc_binary/pal:mqtt_public_broker_smoke
