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
  '  bazel-test                       run every compatible automatic test'
