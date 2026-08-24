#!/bin/sh

printf '%s\n' \
  'GizOS' \
  '' \
  'Usage: make <target> [VARIABLE=value ...]' \
  '' \
  'Configuration:' \
  '  help                             show every public Make target' \
  '  cfg-doctor                       report the local build and H2Loader environment' \
  '' \
  'Bazel:' \
  '  bazel-build                      build every target compatible with the selected config' \
  '  bazel-test                       run every compatible automatic test' \
  '  bazel-test-mqtt_public_broker_smoke run the manual public MQTT broker test' \
  '  bazel-test-gizclaw_h2peer_live_test run the manual GizClaw H2Peer live test' \
  '  bazel-test-gizclaw_pion_live_test run the manual GizClaw Pion live test' \
  '  bazel-coverage-report            generate the repository LCOV and HTML report' \
  '  bazel-release                    build or assemble one closed release slice' \
  '' \
  'H2Loader:' \
  '  h2loader-bin                     build the native h2loader CLI and print its path' \
  '' \
  'Test:' \
  '  test-web                         build and validate Web archives and E2E programs' \
  '' \
  'Guides:' \
  '  guides-build                     build and audit Guides' \
  '  guides-watch                     serve Guides with hot reload' \
  '  guides-preview                   build and preview Guides'
