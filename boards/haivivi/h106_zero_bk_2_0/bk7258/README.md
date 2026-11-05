# H106 Zero BK 2.0

`zero_bk_2_0` is the H106 Zero BK7258 physical board. This BSP selects LED GPIO4 and modem-enable GPIO6. The AP owns the Runtime-facing peripherals and UART1 managed iKCP/logs; the CP owns the BK controller and UART0 download port.

[Board configuration](include/h2_zero_bk_board_config.h) selects the physical identity and revision-specific wiring. [Common definitions](include/h2_zero_bk_board_common_config.h), [AP GPIO defaults](ap/usr_gpio_cfg.h) and [CP GPIO defaults](cp/usr_gpio_cfg.h) define the remaining peripheral and boot contracts. The ST7789 display uses SPI0 on GPIO14–16, with GPIO17 as display DC.

The external QSPI0 LittleFS volume owns `/dl` and `/data`. The [peripheral-power owner](ap/src/h2_zero_bk_peripheral_power.c) retains the shared GPIO13 supply for Flash, audio, NFC and modem. Failed sleep cleanup must not unregister that supply while storage remains mounted. CP defaults retain the AP-initialized output and keep PSRAM powered during runtime and low-voltage sleep; this is not a deep-shutdown or power-loss retention guarantee.

Audio reads both SDK FIFO lanes and extracts the primary microphone lane before mono AEC processing. The additional FIFO lane is not a second product microphone or a measured acoustic reference. Capture stop pauses the SDK reader, waits for borrowed capture/processing/queue work, and retains ownership when pause, drain or destruction fails. The PAL reports the real AEC path before startup; the optional diagnostic observer remains unsupported. MFG uses the shared processed-PCM operator acceptance procedure.

The shared SPI writer serializes LCD and LED transfers and retains the awake vote until SDK completion and FIFO drain. Completed-transfer observation does not prove panel illumination or optical correctness. Layouts retain the fixed Loader/App partition geometry and board-owned recovery inputs.

## Image composition

Final firmware entries select this BSP, its task policies and board-owned layouts. The full-board MFG defaults disable Bluetooth on AP/CP and retain UART management, QSPI storage and display/audio peripherals. They do not use another BK board's SD-card mapping. Public acceptance behavior belongs to [the MFG project](../../../../projects/mfg/README.md); private product factory behavior remains in Firmwares.

The native H2Loader wrapper accepts explicit layout files from `layouts/loader`, `layouts/h2loader` or `layouts/mfg`. Recovery configuration and partition geometry remain board-owned. The minimal recovery Runtime and full Runtime preserve the native Image, Power, identity and console interfaces; initialize and destroy their borrowed providers in the order documented by the public board header.

## Host verification

```bash
bazel test --config=macos_arm64 //boards/haivivi/h106_zero_bk_2_0/...
```

The owning BUILD declares PCM conversion, reader lifetime, AEC initialization/retry, shared supply, CP GPIO policy, task placement, display/SPI and native Power forwarding regressions. Native builds and host fixtures do not establish acoustic, optical, RF, NFC or power-consumption acceptance. Device operations require separately authorized hardware testing.
