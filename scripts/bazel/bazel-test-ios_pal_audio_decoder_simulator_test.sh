#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/ios_application/pal-audio-decoder:ios_pal_audio_decoder_simulator_test \
  --config=ios_sim_arm64
