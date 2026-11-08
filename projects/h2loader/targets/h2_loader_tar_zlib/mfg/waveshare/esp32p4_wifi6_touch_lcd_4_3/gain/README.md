# P4 single-device speaker/microphone gain scan

Build `//projects/h2loader/targets/h2_loader_tar_zlib/mfg/waveshare/esp32p4_wifi6_touch_lcd_4_3:gain_package` with `--config=esp32p4` and temporarily replace the Loader image through H2Loader. Back up the original package and checksums and retain MFG records. Restore the original Loader package afterward and verify both partitions. The MFG gate remains active and existing acceptance records are preserved during calibration. After installation completes and Stage is empty, restart Loader to begin scanning; installation itself does not acquire audio.

Speaker settings are 93, 97 and 100%, with ES8311 ceiling `0xbf`. Microphone settings are 63, 79, 95 and 100%, requesting approximately 24, 30, 36 and 38 dB from ES7210; the highest physical step is 37.5 dB. AEC stays enabled, with dual-microphone lanes 1/3, reference lane 0 and reference digital multiplier 16 unchanged.

Each combination runs silence, the original-strength tone, a five-times-strength tone and silence, for 48 windows. The existing single-device probe uses 500/1000/2500 Hz signals with digital peaks 4478 and 22390. Each window scores 96 actual frames after warmup, at 16 kHz and 512 samples. Keep the environment quiet and device placement fixed. Logs retain only power, peak and clipping statistics.

`analyze.py --log <log> --uid <uid> --version <version>` verifies complete actual ADC/AEC/DAC windows, identity, timing, control readback, background stability, reference activity, 30000 peak headroom and at least 10 dB of total echo-power suppression. Failed combinations retain their reasons. The scan does not persist final gain settings or MFG success; completion records include control restoration.

These observations select candidates under the single-device far-end probe conditions. Final gains still require MFG operator confirmation of near-end speech, double-talk and replay. Digital headroom does not establish low analog amplifier distortion or a global maximum across continuous gain settings.
