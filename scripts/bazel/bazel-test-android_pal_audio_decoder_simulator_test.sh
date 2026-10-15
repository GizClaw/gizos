#!/bin/sh
set -eu
exec scripts/common/bazel_manual_test.py \
  //projects/e2e/targets/android_binary/pal-audio-decoder:android_pal_audio_decoder_simulator_test \
  --config=android_arm64
