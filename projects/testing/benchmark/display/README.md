# Display benchmark firmware

These manual H2Loader App images run once per boot and retain serial management. Use the package for the board and panel fitted to the device.

| Board and panel | Bazel config | Package label suffix | Timing boundary |
| --- | --- | --- | --- |
| Waveshare ESP32-P4 WiFi6 Touch LCD 4.3, ST7701 MIPI DSI | `esp32p4` | `waveshare/esp32p4_wifi6_touch_lcd_4_3:package` | PAL framebuffer copy and cache writeback |
| BK7258 V3, H050IWV parallel RGB | `bk7258` | `beken/bk7258_v3_202405:package` | PAL draw and frame submission, including any buffer availability wait |
| Waveshare ESP32-S3 Touch AMOLED 1.8, SH8601 QSPI | `esp32s3` | `waveshare/esp32s3_touch_amoled_1_8:package` | PAL draw through the BSP's synchronous SPI drain, checked against the existing completed-transfer capture |
| JieLi AC791N DevKit, ILI9481/ILI9488 8080 EMI | `ac791n` | `jieli/jieli_ac791n_devkit:package` | PAL draw and submission distribution, plus one batch from first draw to the final DMA drain inside PAL close |

Build using `bazel build --config=<config> --remote_download_outputs=all //projects/testing/targets/h2_loader_tar_zlib/benchmark/display/<package label suffix>` in the Bazel-managed native SDK environment. Collect the `.update.tar.zlib` App package and `.firmware.json` metadata from `bazel-bin`. Install the matched `role=app` package through GizMoth H2Loader. Collect `H2_DISPLAY_BENCH_*` records after the new App starts.

Each case uses two prebuilt alternating RGB565 checkerboards, eight warm-up updates and 64 timed updates. The two inputs are the full panel and a packed, top-left 128×128 rectangle of 32,768 bytes. Timed windows include the selected driver path's staging and protocol setup. Pattern generation, allocation, logging and statistics run outside those windows. `elapsed_us` in distribution records sums the 64 individual wall-time measurements. P50 and P95 use nearest rank. `logical_payload_mib_s` counts the RGB565 bytes passed at the App boundary, and `submissions_hz` counts returned calls. `payload_mib_s`, `fps` and `updates_hz` use observed DMA completions or the successfully drained EMI batch.

The SH8601's capture counts each stripe after the BSP drains the SPI transaction. A 20-second App deadline reports unavailable if that operation stalls. The EMI batch drains the warm-up before reopening Display, then includes the final close as `close_us`. After a successful suite, the JieLi App reopens Display, submits a full-screen checkerboard and leaves the backlight on. `H2_DISPLAY_BENCH_PREVIEW` records that post-run submission separately. The per-update transfer boundary and physical scanout cadence are unavailable through the corresponding public APIs. The P4 and BK panels still scan full rasters after region updates. Physical screen refresh and optical presentation require separate measurement.

## Local WASM preview

```sh
bazel run //projects/testing/targets/pkg_tar/benchmark/display:serve -- --host 127.0.0.1 --port 8170
```

Open `http://127.0.0.1:8170/` and click **Start**. The Web Display PAL draws the benchmark’s two full-screen checkerboards on a 480 × 320 canvas. The preview holds each pattern for one second so both are visible. **Stop** freezes the last frame. Reload to play again. Hardware timings are available from the native benchmark above.
