# BK7258 Board

BK7258 board profile for the C firmware tree.

- full-size A/B loader/app window: `primary_cp_app=1360K`, `primary_ap_app=2380K`, `s_app=3740K`
- AP pref storage: SDK EasyFlash AP tail storage
- reserved FlashDB KV area: offset `0x780000`, size `128 KiB`
- coredump area: offset `0x7a0000`, size `360 KiB`
- `/dl`: required SD/FATFS directory `<sd-root>/h2loader/dl`
- `/data`: required SD/FATFS directory `<sd-root>/h2loader/data`
- audio: 16 kHz mono speaker, two microphone channels, 320 samples per frame
- ADC buttons: previous, next, and menu ranges from the board BSP profile

The board requires an inserted SD card for h2loader-managed app flows. Missing SD,
unsupported filesystem, or mount/prepare failure is a startup error; there is no
internal-flash fallback for `/dl` or `/data`.
