#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/testing/targets/pkg_tar/pal-audio-decoder:wasm_pal_audio_decoder_browser_test
