# ESP-Mosaico / ESP32-S31

CoreBoard V1.2 board composition for the pinned ESP-IDF 6.2 baseline. Sources
implement PAL wiring, default SC101IOT Camera injection and Type-C OTG CDC
Loader integration.

The CST9220 touch provider continues reading while a contact is active or a report needs retrying, including when the IRQ line has returned high. Fresh stationary contact reports preserve a held finger. A contact with no valid report for 500 ms is released at its last coordinates so a skipped release cannot leave pointer clients pressed indefinitely.

This delivery supports PAL E2E execution on Mosaico. The accepted device-suite results and their image-specific limits are recorded in [GizClaw/gizos#708](https://github.com/GizClaw/gizos/issues/708). They do not qualify every physical peripheral or a complete product. Lingxi and OneNET remain outside acceptance.

Use the maintained `IDF_PATH` / `IDF_TOOLS_PATH` and build configuration:

```sh
bazel test //boards/espressif/mosaico/esp32s31:camera_test \
  //boards/espressif/mosaico/esp32s31:usb_test
bazel build --config=esp32s31 \
  //projects/h2loader/targets/h2_loader_tar_zlib/loader/espressif/mosaico:package
```

Applications depend directly on `//boards/espressif/mosaico/esp32s31` and the board
layout; no `e2e-app/esp_mosaico` package is required. Ownership and validation boundaries are described below.

Only the user's left-slot SC101IOT, 1280x720 UYVY, is in scope. Camera provider
injection does not probe or start capture. AEC calibration is limited to the controlled fixture conditions; no product
calibration is applied. OV3640 and other product features remain outside that acceptance.

The board exposes [read-only storage usage queries](esp32s31/include/h2_mosaico_storage.h) for product workers. Internal Flash usage combines current image lengths, allocated LittleFS blocks and reserved boot/partition/metadata regions. NAND usage uses mapped logical pages and usable FTL capacity through the pinned [SPI NAND extension](../../../third_party/spi_nand_flash_patch/README.md). It initializes NAND lazily, borrows an existing handle when present, and releases only handles it initialized after queries have joined. This adds statistics, not a NAND filesystem or device acceptance.

## Ownership and test discovery

- `boards/espressif/mosaico/esp32s31` owns physical device wiring, the existing PAL
  driver implementations and generic USB CDC transport.
- [`boards/espressif/mosaico/web`](web/README.md) owns the browser board metadata, physical button names and reusable enclosure skin.
- Portable H2Loader protocol/session logic is owned by
  `libs/h2loader/device`. Final firmware entries use the common Device service
  through `Runtime.io_stream`; board and native providers own TinyUSB setup and
  complete-frame write arbitration.
- `projects/testing` owns the reusable E2E Apps, including GizClaw,
  atomic, Lua, performance and business Apps. Enumerate all of them with
  `//projects/testing/tools:list_apps`; do not filter only `pal-*` names.
- The product Loader is not an E2E App entry.

There is no `peripherals` firmware suite. Driver regression is grouped by
button/registry, IMU, battery and lifecycle under the board `tests/` directory.
No standalone motor pulse validation is scheduled. See
`esp32s31/tests/README.md` for the regression target mapping.
