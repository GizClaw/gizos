#!/bin/sh

printf '%s\n' \
  'GizOS' \
  '' \
  'Usage: make <target> [VARIABLE=value ...]' \
  '' \
  'Configuration:' \
  '  help                             show every public Make target' \
  '' \
  'Bazel:' \
  '  bazel-build                      build every target compatible with the selected config' \
  '  bazel-test                       run every compatible automatic test' \
  '  bazel-test-mqtt_public_broker_smoke run the manual public MQTT broker test' \
  '  bazel-test-gizclaw_h2peer_live_test run the manual GizClaw H2Peer live test' \
  '  bazel-test-gizclaw_pion_live_test run the manual GizClaw Pion live test' \
  '' \
  'Test:' \
  '  test-web                         build and validate Web archives and E2E programs'
