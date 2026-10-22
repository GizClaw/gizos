# ESP-Mosaico / ESP32-S31

CoreBoard V1.2 board composition for the pinned ESP-IDF 6.2 baseline. Sources
implement PAL wiring, default SC101IOT Camera injection and Type-C OTG CDC
Loader integration. The bare-chip profile is `boards/esp32s31_chip/esp32s31`.
It does not depend on this board or its BSP.

Hardware qualification is **pending**. Do not infer a physical PASS from a host
unit test, a firmware build or historical receipts. Hardware execution remains
paused under issue #708.

Use the maintained `IDF_PATH` / `IDF_TOOLS_PATH` and build configuration:

```sh
bazel test //boards/esp_mosaico/esp32s31:camera_test \
  //boards/esp_mosaico/esp32s31:usb_test
bazel build --config=esp32s31 \
  //projects/e2e/targets/esp_idf_firmware/reference-smoke/esp32s31_chip:firmware
bazel build --config=esp32s31 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/esp_mosaico:package
```

Applications depend directly on `//boards/esp_mosaico/esp32s31` and the board
layout; no `e2e-app/esp_mosaico` package is required. See
`guides/zh/developing/esp_mosaico.md` for ownership and validation boundaries.

Only the user's left-slot SC101IOT, 1280x720 UYVY, is in scope. Camera provider
injection does not probe or start capture. AEC effects, SPI NAND filesystem,
OV3640, other modules and product behavior are outside this delivery.
