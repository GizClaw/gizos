# AMOLED display investigation — 2026-09-11

Worktree: `/Users/grez/.codex/worktrees/bb57/gizos`, branch `lua-fishing-game`,
base commit `3a1a01dab1d56613f7f9d402667759e3dc06130f`.

## Confirmed baseline

Fishing Game, Bloomspeaker and Flappy Bird use the shared SH8601 QSPI display
provider and SPI DMA. DMA was already enabled in the failing fishing baseline.
The provider copies each rectangle into an internal DMA buffer and drains the
pending color transfer before reusing that buffer. The local ESP-IDF 6.0
`esp_lcd_panel_io_tx_param(..., -1, NULL, 0)` implementation drains in-flight
transactions even with the negative command. DMA buffer reuse is therefore not
an established explanation for the observed artifacts.

Fishing uses retained 16-pixel dirty tiles. CPU source stride conversion and
the SH8601 driver's exclusive end coordinates were inspected; no mismatch was
identified. This inspection is not proof that the complete display path is correct.

## v16: TE synchronization experiment failed

`fishing-te-v16` preserves 80 MHz QSPI and enables rising-edge GPIO13 TE waiting
once before the first rectangle of a frame, with a 34 ms timeout. Other apps
remain opt-out. Build and host display-config/Lua unit tests passed.

Built using the local ESP-IDF 6.0 checkout and component mirror under
`/Users/grez/haivivi/firmware-devenv`. The mirror needs its existing localhost
HTTP service on port 8765; a machine restart stops that service.

Installed through H2Loader on `/dev/tty.usbmodem1101` at host baud 115200:

- Device UID: `94a990281a12`, board `amoled`, target `esp32s3`.
- Update package SHA-256:
  `1fafc96e8305f6de057f602abba4c7f64cc2510001baba9a1286a4137cc7cc07`.
- App image SHA-256:
  `bc84c1ea93ec09b3101d17e61679bb9c4ebec3e666f49bc7b3efad94175cf729`.
- Post-upgrade status: active version `fishing-te-v16`, partition 2,
  `stage_valid=0`, `last_result=0`; recovery loader partition 1 remains valid
  with unchanged image checksum
  `5ad3716ee7272661a4fe97eee7d3dc3adaf997f61060a0a3be6e466552b49afb`.
- Game READY, touch, casting, flight and waiting logs observed; no unexpected
  reset observed during approximately 150 seconds after app boot.
- TE waits/timeouts: **660/660**, no successful wait. Typical wait ~33–34 ms;
  first startup wait was ~189 ms. The experiment did not obtain a usable TE
  event; it does not establish whether the cause is wiring, panel setup or ISR.
- Ready scene ~7.4–8.6 FPS; 51-node waiting ~3.6–4.0 FPS after settling.
  Waiting present cost ~44–48 ms. This is slower than the v15 baseline due to
  the ineffective wait. Occasional touch read error -4 still recovers.
- User explicitly confirmed horizontal displacement persists and supplied a
  new photograph showing broken/shifted rod and line pixels.

**Do not report v16 as a visual fix.** TE vs QSPI timing vs region-addressing
causes remain unresolved. Next controlled configuration disables ineffective
TE waits and compares 40 MHz QSPI against the 80 MHz/no-wait v15 baseline,
without changing the artwork, DMA or retained tile renderer.

## v17: 40 MHz comparison installed; user confirms displacement gone

`fishing-qspi40-v17` configures 40 MHz QSPI and disables TE waiting. Firmware
build and `git diff --check` passed. The existing v16 host-test result covers
the unchanged common implementation; the only v17 code change is target config.

- Package size 1239770 bytes, SHA-256
  `767ebcbf35280ba921066194f6879373cec88497c5a38aff3b7a3c96f13da0be`.
- App image SHA-256
  `9c9801981f9fd28f9cad61649471b40e5577516c479635590a59b99d1d8ed178`.
- H2Loader send completed with matching checksum on `/dev/tty.usbmodem1101`.
- `reboot upgrade` returned `result=accepted`. Unlike v16, the subsequent
  monitor did not show boot or install-completion logs. The final old-app line
  was `spi_bus_initialize: SPI bus already initialized` (also seen during the
  successful v16 reboot, so this line alone does not identify the failure).
- Stopped the host monitor; a fresh status request timed out with code -6.
  A separate five-second raw read received zero USB bytes. No further firmware
  write, raw flash, erase, or device reset was performed.

After the user reconnected the same port, status confirmed active version
`fishing-qspi40-v17` and the matching app/package checksums above, partition 2,
`stage_valid=0`, `last_result=0`. Recovery loader partition 1 remains valid with
its unchanged checksum. No repeat upload was needed.

Runtime logs include ready, casting, release and landing (13.39 m / 11.90 m
line / 51 nodes), then waiting. Ready samples were ~10.7–12.2 FPS (one 9.8 FPS
sample), present ~11–15 ms; casting present ~19 ms. Early settled waiting samples
were ~4.0–4.1 FPS, present ~14–19 ms. No TE timeouts or unexpected reset appeared
in the sampled runtime logs. Occasional touch read error -4 still recovers.
Later waiting samples reached ~4.2–4.7 FPS, with simulation ~135–146 ms,
draw often ~49–59 ms and present ~11–19 ms. Native `advance_rope` accounts
for ~111–120 ms in representative samples. Frame-rate goals remain unmet.

After being asked to cast or scroll equipment on v17, the user reported
"没有了，但是帧率还能继续提升么" — horizontal pixel displacement is no longer
visible in their test. **Installation, runtime and user visual verification
passed for this configuration.** This does not establish long-duration display
reliability across all scenes.

Compared with v15 (80 MHz, no TE wait), v17 retains DMA and dirty-tile rendering
but lowers the display clock to 40 MHz. The results implicate the 80 MHz display
transfer path/timing margin, not absence of DMA; the exact electrical or panel
timing failure has not been measured. Keep 40 MHz as the verified configuration
while optimizing CPU-side simulation and drawing. No performance optimization
has yet been implemented in response to the user's follow-up question.
