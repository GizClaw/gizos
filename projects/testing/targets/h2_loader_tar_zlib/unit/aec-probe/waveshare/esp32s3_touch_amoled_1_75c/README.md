# 1.75C single-device echo and gain probe

This independent H2Loader App reuses the original H200 single-device probe's three-tone signal and silence-baseline scan. Real Audio PAL simultaneously plays, captures and observes raw ADC, actual AEC reference, processed PCM and completed DAC writes. It does not start GizClaw or inject microphone PCM.

```sh
. /path/to/firmwares-devenv/export.sh
bazel build --config=esp32s3 \
  //projects/testing/targets/h2_loader_tar_zlib/unit/aec-probe/waveshare/esp32s3_touch_amoled_1_75c:package
```

Use managed H2Loader installation after verifying board and device UID. The device emits intermittent calibration tones for approximately three minutes; keep speaker/microphone placement and the environment stable. Raw serial logs bind App version, UID and a random run ID. On completion, stop and join audio, close the track and remove the observer before confirming the App. Failures retain their errors; completed measurement does not imply every candidate is valid.

Four actual direct-AEC profiles cover single/dual microphone and 0/12 dB reference PGA. Each profile records ten ordered silence/far-only windows across combinations of 75/100% speaker and approximately 18/24/30 dB microphone gain, forty windows overall. PAL microphone percentages are read back at the codec's actual quantized values. Microphone adjustments do not change reference PGA. Running the probe does not persist changes to board defaults. Each far window warms up for 96 frames and scores 96; silence warms up for 32 and scores 64, at 16 kHz with 512 samples. Checks cover complete actual DAC frames, raw input, AEC output and real-time cadence. Logs retain power, peak and clipping statistics without microphone PCM.

```sh
python3 projects/testing/targets/h2_loader_tar_zlib/unit/aec-probe/waveshare/esp32s3_touch_amoled_1_75c/analyze.py \
  --log /absolute/path/serial.log --uid <device-uid> --version <image-version>
```

The analyzer rejects incomplete identity, timing, channel, window or DAC evidence. It independently checks silence-background stability, signal/noise, reference activity, 30000 peak headroom and at least 10 dB of background-subtracted far-only echo-power reduction. `far_echo_probe_passed` applies only to this single-device signal and level; `acoustic_qualified=false` always remains. Digital headroom does not prove low analog amplifier/speaker distortion. Real near-end retention and double-talk require separate measurements. Full automatic acceptance uses `projects/testing/unit/aec-calibration` with its independent real near-end acoustic fixture and does not inherit this probe's partial success.

After selecting parameters from actual logs, change the BSP, rebuild and verify the relevant Audio/GizClaw images on hardware. Temporary probe profiles or success from an older image cannot establish validation of new defaults.
