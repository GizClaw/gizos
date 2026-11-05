# ESP-Mosaico / ESP32-S31

CoreBoard V1.2 board composition for the pinned ESP-IDF 6.2 baseline. Sources
implement PAL wiring, default SC101IOT Camera injection and Type-C OTG CDC
Loader integration.

This delivery supports PAL E2E execution on Mosaico. The accepted device-suite results and their image-specific limits are recorded in [GizClaw/gizos#708](https://github.com/GizClaw/gizos/issues/708). They do not qualify every physical peripheral or a complete product. Lingxi and OneNET remain outside acceptance.

Use the maintained `IDF_PATH` / `IDF_TOOLS_PATH` and build configuration:

```sh
bazel test //boards/espressif/mosaico/esp32s31:camera_test \
  //boards/espressif/mosaico/esp32s31:usb_test
bazel build --config=esp32s31 \
  //projects/h2loader/targets/loader/espressif/mosaico:package
```

Applications depend directly on `//boards/espressif/mosaico/esp32s31` and the board
layout; no `e2e-app/esp_mosaico` package is required. Ownership and validation boundaries are described below.

Only the user's left-slot SC101IOT, 1280x720 UYVY, is in scope. Camera provider
injection does not probe or start capture. AEC calibration is limited to the controlled fixture conditions; no product
calibration is applied. SPI NAND, OV3640 and other product features are excluded.

## Ownership and test discovery

- `boards/espressif/mosaico/esp32s31` owns physical device wiring, the existing PAL
  driver implementations and generic USB CDC transport.
- Portable H2Loader protocol/session logic is owned by
  `libs/h2loader/device`. Final firmware entries use the common Device service
  through `Runtime.io_stream`; board and native providers own TinyUSB setup and
  complete-frame write arbitration.
- `projects/e2e/apps` owns the reusable E2E Apps, including GizClaw,
  atomic, Lua, performance and business Apps. Enumerate all of them with
  `//projects/e2e/tools:list_apps`; do not filter only `pal-*` names.
- The product Loader is not an E2E App entry.

There is no `peripherals` firmware suite. Driver regression is grouped by
button/registry, IMU, battery and lifecycle under the board `tests/` directory.
No standalone motor pulse validation is scheduled. See
`esp32s31/tests/README.md` for the regression target mapping.
