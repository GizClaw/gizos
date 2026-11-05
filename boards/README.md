# Boards

Physical board support is grouped by manufacturer, then by board model:

```text
boards/
├── beken/
│   └── bk7258_v3_202405/
├── espressif/
│   ├── esp32p4_func_ev_board_v1_4/
│   ├── esp32s3_devkitc_1/
│   └── mosaico/
├── jieli/
│   └── jieli_ac791n_devkit/
├── jlc/
│   └── esp32s3_szp/
└── waveshare/
    ├── esp32p4_wifi6_touch_lcd_4_3/
    ├── esp32s3_a7670e_4g/
    ├── esp32s3_touch_amoled_1_75c/
    └── esp32s3_touch_amoled_1_8/
```

Use lowercase company directory names. Waveshare directories use the full model
name without a repeated manufacturer prefix. Physical boards belong to their
board manufacturer, even when another company supplies the chip. For example,
[ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm)
belongs to Waveshare, [Mosaico](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/index.html)
belongs to Espressif, and [SZP ESP32-S3](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/)
belongs to JLC.

`esp32s3_devkitc_1` is the Espressif ESP32-S3-DevKitC-1. The current BSP uses
GPIO48 for its RGB LED, as on the initial revision; the
[v1.1 guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html)
specifies GPIO38. The model directory does not imply a PCB revision.
`esp32s3_szp` is JLC's ShiZhanPai (SZP) ESP32-S3 development board; its official
product documentation lists the ESP32-S3-WROOM-1-N16R8 module.

Each board owns its chip or target directories:

```text
boards/<company>/<board>/
├── BUILD.bazel                 # Board version and public board targets
├── <chip-or-target>/
│   ├── BUILD.bazel             # Native component and board tests
│   ├── include/                # BSP public API
│   ├── src/                    # Board wiring and private implementation
│   └── layouts/<profile>/      # Firmware configuration and flash layout
└── web/                        # Optional browser board and device skins
```

The company and model directories are part of source paths and public Bazel
labels. For example, the 1.8-inch AMOLED board uses
`//boards/waveshare/esp32s3_touch_amoled_1_8:version` and
`//boards/waveshare/esp32s3_touch_amoled_1_8/esp32s3`. Runtime board identifiers,
package identities and project target names retain their existing values, such
as `amoled`, `devkit` and `szp`. Historical qualification evidence keeps the paths
recorded for the build it describes.
