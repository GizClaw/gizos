# BK7258 Board

BK7258 board profile for the C firmware tree.

Loader uses UART and BLE for management. Its [AP](layouts/loader/ap.defaults) and [CP](layouts/loader/cp.defaults) overrides disable Classic Bluetooth, selecting the SDK's BLE-only host and controller libraries within the fixed Loader window.

- fixed Loader window: CP `1156 KiB` + AP `1224 KiB` = `2380 KiB`
- independently linked App window: `5100 KiB` at physical `0x264000`
- App boot record: `0x77f000`, `4 KiB`; native startup always selects Loader, whose CP enters a pending trial or a confirmed App
- AP pref storage: SDK EasyFlash AP tail storage
- reserved FlashDB KV area: offset `0x780000`, size `128 KiB`
- coredump area: offset `0x7a0000`, size `360 KiB`
- `/dl`: required SD/FATFS directory `<sd-root>/h2loader/dl`
- `/data`: required SD/FATFS directory `<sd-root>/h2loader/data`
- audio: 16 kHz mono speaker, two microphone channels, 320 samples per frame
- ADC buttons: K3..K6 / KEY1..KEY4 use P12 / ADC14 through J27. The [official EVB V3 hardware design package](https://docs.bekencorp.com/HW/BK7258/HDK-BK7258_QFN88_9X9.zip), main-board schematic sheet 2, specifies R14=20 kOhm external pull-up and switched resistances 0/6.8/20/62 kOhm, with ADC annotations 0/0.6/1.2/1.8 V. J27 must connect the ladder to P12; keep internal pulls disabled. Key recognition uses calibrated native raw-code intervals, without assuming a 4096-count full scale or reporting unverified voltages. AP configures the pad while CP services acquired ADC transactions. P28 is the UVC power-enable signal, not the key input.
- optional GT911/GT1151 touch: after address selection, INT returns to input mode and settles before controller detection/configuration; the AP adapter preserves native geometry, reads back configured ranges and axis-exchange state, and maps coordinates to the actual display extent

The board requires an inserted SD card for h2loader-managed app flows. Missing SD,
unsupported filesystem, or mount/prepare failure is a startup error; there is no
internal-flash fallback for `/dl` or `/data`.
