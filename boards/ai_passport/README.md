# FoloToy AI Passport

This in-tree board and its Loader/MFG entries are part of the expanded
[PR 771](https://github.com/GizClaw/gizos/pull/771) scope. The board owns wiring
and the product layout; `projects/mfg/apps/ai_passport` owns the ten-step profile,
phase resource policy and encrypted recording store. Reusable libraries and ESP
providers remain free of board pins and partition choices. Downstream Firmwares
can still compose the public C3 integration without depending on these images.

The MFG owner uses the checked ESP Wi-Fi stop API to release driver/netif memory
between radio and audio phases, including the Wi-Fi-to-BLE transition in a single
checklist. It quiesces clients and serializes ownership before stopping, preserves
saved credentials, rejects a stop during active SoftAP use, and retries incomplete
cleanup. This lifecycle is required by the no-PSRAM target's memory budget;
completed checklists do not establish endurance or repeated-cycle guarantees.

ESP32-C3 at 160 MHz, 8 MiB Flash, no PSRAM. The board composes public GizOS
ESP-IDF providers with the product wiring; the shared manufacturing session is
at `//projects/mfg/apps/ai_passport:package` (`--config=esp32c3`). The ordinary
Loader is `//projects/h2loader/targets/h2loader_tar_zlib/loader/ai_passport:package`;
it uses the same board and layout without starting MFG. This first-install
Loader starts USB management only, keeping the C3's internal RAM available for
package verification and installation. MFG polls the keys and battery directly
and reserves one unused Runtime input slot; the first-install Loader reserves
four. Both reserve eight component mappings; MFG queues four Runtime events
and first installation queues eight. They request image-owned USB command
stacks: 32 KiB for first installation and 20 KiB for MFG. On 0.1.34, live JTAG stack-pattern measurements after managed
update and AEC creation showed peaks of 14,384 bytes on main and 12,252 bytes
on USB, from their original 32 KiB reservations. MFG 0.1.35 selected 24 KiB for
each; 0.1.37 selects 20 KiB and keeps the 8 KiB microphone DSP and 4 KiB speaker
stacks. Version 0.1.36 also budgets 1 KiB each for native serial RX and framed
stream RX, with an eight-segment advertised receive window. The stream keeps
normal backpressure, retransmission, complete payload lengths and checksums;
shared defaults remain 8 KiB, 4 KiB and twenty segments. Subsequent device paths
require their own validation. Other images keep the shared 48 KiB command default.
MFG starts its BLE service only for the BLE check, after releasing audio and
Wi-Fi. Its BLE server executes commands on its own 32 KiB stack rather than
starting another command task. Phase changes wait without holding command
locks across BLE joins. USB recovery remains available.

| Peripheral | Wiring / logical identity |
| --- | --- |
| ST7789 240x320 | SPI2 MOSI 9, SCLK 8, CS 1, DC 20, no reset; backlight PWM 21 |
| ADC keys | GPIO0 / ADC1 channel 0; UP 102 below 150 mV, DOWN 101 below 447 mV, OK 103 below 1900 mV; radio group 201 |
| I2C | SDA 10, SCL 7, 100 kHz; ES8311 0x18, CW2017 0x63 |
| Audio I2S0 | MCLK 6, BCLK 5, WS 3, DOUT 2, DIN 4; no PA GPIO |
| Battery | CW2017, input 401; read-only voltage and SOC, no profile programming |
| Console | Native USB Serial/JTAG |

Wiring follows the [FoloToy BSP](https://github.com/FoloToy/ai-passport/tree/3d685a90bdb96dd8526d84edde1e63fec25b460e/components/bsp)
and the existing Firmwares AI Passport integration. Audio uses 16 kHz mono,
512-sample frames, MCLK multiple 384, five 128-frame DMA descriptors and real
ESP-SR C3 AEC. Each native frame consumes four complete DMA descriptors, avoiding
IDF's partial-descriptor cursor replacement under queue pressure. Total DMA
payload capacity remains 640 frames (40 ms) per direction. After audio users
stop, the MFG entry explicitly selects raw
capture for MIC and enabled AEC for its separate check; raw capture reports
AEC disabled and does not claim echo-cancellation acceptance. Codec and ADC
services initialize lazily. Checked audio shutdown releases I2S, codec and DSP
memory before radio checks. Display borrows each RGB565 strip synchronously and
uses a four-row DMA staging buffer. MFG likewise draws four-row strips and
uses a two-frame playback queue; frame completeness, cadence and clipping
requirements remain enforced. Checked target cleanup also releases DSP memory
on a failed check before saving its result.

The H2Loader layout preserves the existing product geometry: Loader at
0x20000 (2 MiB), App at 0x220000 (3 MiB), preferences at 0x520000 (256 KiB),
coredump at 0x560000 (64 KiB), data at 0x570000 through 0x800000. `/dl` and
`/data` share the data partition, mounted under `/passport`; first-use formatting
is restricted to a completely erased partition. Existing data is not formatted
on a mount error. Preferences use the public provider's erased-only migration.

The MFG image uses a 4 KiB shared safe-I/O scratch buffer while preserving 16 KiB
preference values and full disk transfer lengths. Its GATT schema reserves one
service with two characteristics; shared defaults remain six with three each.
AEC recording keeps every native PCM sample and the shared acoustic thresholds.
The owner encrypts temporary frames with AES-128-GCM into at most 256 KiB at the
unused tail of the inactive P2 Loader candidate (App-relative 0x2c0000). Before
writing, it verifies both Loader metadata and the actual ESP image boundary,
partition geometry and running partition. An App candidate, staged update,
invalid image or overlapping image blocks recording. Keys exist only in RAM;
retries renew nonces, and checked close destroys keys and erases the scratch.
No image bytes, partition geometry, preferences or data filesystem are moved.
The owner pumps audio and recording work between display strips; measured frame
counts, cadence, clipping and echo-reduction thresholds still govern AEC.

Archive installation uses allocator-owned streaming workspace rather than large
stack arrays. Manual P1 recovery holds Loader command mode before seeding a
changed/unknown active identity; stale Stage/P2 images remain available for
inspection or explicit abort. Normal managed AUTO updates keep their behavior.

The supplier BSP exposes only the three ADC keys, so POWER is a separate
operator-confirmed off/on check rather than an invented GPIO input. The board
has no MCU interface to its passive NFC tag. Wi-Fi connection policy,
acceptance results and image task budgets belong to the application. Build and
host-test results do not qualify physical display, keys, battery, RF or acoustics.
