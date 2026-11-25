# FoloToy AI Passport

This in-tree board and its Loader/MFG entries are part of the expanded
[PR 771](https://github.com/GizClaw/gizos/pull/771) scope. The board owns wiring
and the product layout; `projects/h2loader/targets/h2_loader_tar_zlib/mfg/folotoy/ai_passport` owns the ten-step profile,
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
at `//projects/h2loader/targets/h2_loader_tar_zlib/mfg/folotoy/ai_passport:package` (`--config=esp32c3`). The ordinary
Loader is `//projects/h2loader/targets/h2_loader_tar_zlib/loader/folotoy/ai_passport:package`;
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
require their own validation. Other images retain their own command-stack defaults.
MFG starts its BLE service only for the BLE check, after releasing audio and
Wi-Fi. Its BLE server executes commands on its own 32 KiB stack rather than
starting another command task. Phase changes wait without holding command
locks across BLE joins. USB recovery remains available.

## Supported hardware definition

This BSP supports the ESP32-C3, 8 MiB Flash, no-PSRAM FoloToy AI Passport hardware map in the vendor's [bsp_pins.h](https://github.com/FoloToy/ai-passport/blob/3d685a90bdb96dd8526d84edde1e63fec25b460e/components/bsp/include/bsp_pins.h), pinned to commit `3d685a90bdb96dd8526d84edde1e63fec25b460e`. This source-pinned definition is the compatibility boundary; a numeric PCB silkscreen revision was not established during device acceptance. Units with a different panel, key ladder, wiring or audio lane order require separate configuration and qualification.

| Peripheral | Supported hardware / GizOS configuration |
| --- | --- |
| LCD | ST7789P3, 240 × 320, four-wire SPI2, RGB565; MOSI GPIO9, SCLK GPIO8, CS GPIO1, DC GPIO20; reset is not MCU-connected (`-1`, software reset); SPI mode 0, inversion enabled |
| Backlight | GPIO21, LEDC low-speed timer 0 / channel 0, 5 kHz, 10-bit duty |
| Key ADC | GPIO0 / ADC1 channel 0, calibrated millivolts with 12 dB attenuation; radio group 201 |
| UP | Physical ladder window `0 <= mV < 150`; logical button 102 |
| DOWN | Physical ladder window `150 <= mV < 447`; logical button 101 |
| OK | Physical ladder window `447 <= mV < 1900`; logical button 103 |
| Keys released | `mV >= 1900`; valid samples pass through the two-sample ADC stabilizer |
| I2C | Port 0, SDA GPIO10, SCL GPIO7; GizOS uses 100 kHz; ES8311 address `0x18`, CW2017 address `0x63` (both 7-bit) |
| Audio I2S0 | MCLK GPIO6, BCLK GPIO5, WS GPIO3, MCU-to-codec DOUT GPIO2, codec-to-MCU DIN GPIO4; no MCU-controlled PA (`-1`) |
| Audio receive | Philips I2S, stereo S16LE on the wire: raw lane 0 is the microphone ADC, raw lane 1 is the DAC playback reference; mono processed output |
| Battery | CW2017, logical input 401; read-only voltage and SOC, no profile programming |
| Console | ESP32-C3 native USB Serial/JTAG |
| POWER / NFC | POWER is an operator-confirmed off/on switch without an MCU button mapping; the passive NFC tag has no MCU interface |

The vendor [button implementation](https://github.com/FoloToy/ai-passport/blob/3d685a90bdb96dd8526d84edde1e63fec25b460e/components/bsp/src/bsp_button.c) confirms half-open calibrated voltage windows. Its [display implementation](https://github.com/FoloToy/ai-passport/blob/3d685a90bdb96dd8526d84edde1e63fec25b460e/components/bsp/src/bsp_display.c) identifies the ST7789P3 panel. GizOS deliberately selects a 40 MHz LCD clock instead of the vendor's 80 MHz default; wiring compatibility does not copy the vendor's task, DMA or clock policy.

The [board audio configuration](esp32c3/src/h2_esp_board_peripherals.c) sets `raw_channels=2`, `mic_channel_index=0`, `ref_channel_index=1` and `processed_channels=1`. The [ESP audio provider](../../../native_component_src/esp-idf6.x/h2_es8311_audio_system/src/h2_esp_es8311_platform_audio.c) enables the codec DAC reference through ES8311 register `0x44 = 0x58`; the [SR adapter](../../../native_component_src/esp-idf6.x/h2_es8311_audio_system/src/h2_esp_es8311_sr.c) feeds raw sample `2*i` to the microphone input and `2*i+1` to the reference input. AEC uses this separate captured reference lane; the basic MIC check selects raw microphone processing with AEC disabled.

Audio uses 16 kHz mono,
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

The [H2Loader layout contract](esp32c3/layouts/h2loader/README.md) defines
image limits, recording admission and the complete Flash map: Loader at
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
