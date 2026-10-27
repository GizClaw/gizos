# ESP-Mosaico / ESP32-S31

CoreBoard V1.2 board composition for the pinned ESP-IDF 6.2 baseline. Sources
implement PAL wiring, default SC101IOT Camera injection and Type-C OTG CDC
Loader integration. The bare-chip profile is `boards/esp32s31_chip/esp32s31`.
It does not depend on this board or its BSP.

This delivery supports PAL E2E execution on Mosaico. The accepted device-suite
results and their image-specific limits are recorded in issue #708 and
`guides/zh/developing/esp_mosaico_e2e.md`. They do not qualify every physical
peripheral or a complete product. Lingxi and OneNET remain outside acceptance.

Use the maintained `IDF_PATH` / `IDF_TOOLS_PATH` and build configuration:

```sh
bazel test //boards/esp_mosaico/esp32s31:camera_test \
  //boards/esp_mosaico/esp32s31:usb_test
bazel build --config=esp32s31 \
  //tools/bazel/smoke/esp32s31:firmware
bazel build --config=esp32s31 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/esp_mosaico:package
```

Applications depend directly on `//boards/esp_mosaico/esp32s31` and the board
layout; no `e2e-app/esp_mosaico` package is required. See
`guides/zh/developing/esp_mosaico.md` for ownership and validation boundaries.

Only the user's left-slot SC101IOT, 1280x720 UYVY, is in scope. Camera provider
injection does not probe or start capture. AEC calibration is limited to the controlled fixture conditions; no product
calibration is applied. SPI NAND, OV3640 and other product features are excluded.

## Ownership and test discovery

- `boards/esp_mosaico/esp32s31` owns physical device wiring, the existing PAL
  driver implementations and generic USB CDC transport.
- H2Loader protocol injection is owned by
  `projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_mosaico_usb`;
  the board does not depend on the product Runtime.
- `projects/e2e/apps` owns the reusable E2E Apps, including GizClaw, libco,
  atomic, Lua, performance and business Apps. Enumerate all of them with
  `//projects/e2e/tools:list_apps`; do not filter only `pal-*` names.
- The S31 `reference-smoke` target is tagged `compile-only`. The product Loader
  and chip compiler check are not E2E App entries.

There is no `peripherals` firmware suite. Driver regression is grouped by
button/registry, IMU, battery and lifecycle under the board `tests/` directory.
No standalone motor pulse validation is scheduled. See
`esp32s31/tests/README.md` for the regression target mapping.
