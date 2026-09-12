# H106 firmware integration

The portable game, Lua input policy and native 240 × 240 raster path live in GizOS. The private `tiga_esp_v4_2` BSP remains in Firmwares; it is not copied into GizOS's board registry. `h106-firmware-integration.patch` contains the downstream launcher, the pinned BSP's unused cellular-location configuration compatibility change, and the existing display-driver optimizations (64-row synchronous DMA chunks and paired RGB565 byte conversion) with their tests. SPI stays at 40 MHz. The patch does not change Loader or partition layout.

The integration patch is based on Firmwares commit `cf8bfa061`. Apply it to a clean, separate checkout of that revision, then build against this GizOS working tree using Bazel's `--override_module=gizos=<absolute-gizos-path>`. The latest builds use the separate archive `/tmp/fishing-h106-migrate.ZmdGqC`, not a Git worktree; the user's private checkout was not modified. The native build uses configured ESP-IDF 6.0 and its local component cache. The patch was reverse-checked against the complete staged launcher and modified BSP files. The launcher resolves public component paths through `H2_GIZOS_ROOT`; the private ownership gate passes without weakening its checks.

```sh
bazel build --config=esp32s3 \
  --override_module=gizos=<absolute-gizos-path> \
  --define=FISHING_H106_INPUT_TEST=1 \
  --@gizos//tools/bazel:firmware_version=0.1.0-fishing-h106.28-test \
  //projects/h106/targets/h2loader_tar_zlib/lua-fishing-game/tiga_esp_v4_2:package
```

Omit `--define=FISHING_H106_INPUT_TEST=1` for normal firmware; give it a distinct version. The test flag registers a diagnostic-only Runtime input bridge. After successful automation the exclusive test session closes and physical input resumes; normal firmware never registers the bridge.

The package is `bazel-bin/projects/h106/targets/h2loader_tar_zlib/lua-fishing-game/tiga_esp_v4_2/package/tiga_esp_v4_2-lua-fishing-game-esp32s3.update.tar.zlib`. Use H2Loader `status` to verify the board, App role and available commands. The independent Fishing Game App supports `send --file <package>` followed by `reboot upgrade --monitor` directly; do not unnecessarily reboot into the installed MFG Loader. Require the successful package checksum before upgrade. Never use the generated factory image for a normal App upgrade. Only one serial client may operate at a time.

From GizOS, the bounded test collector can install an already-staged test package and monitor the device:

```sh
python3 projects/example/apps/lua-fishing-game/tools/verify_h106_buttons.py \
  --binary bazel-bin/projects/h2loader/targets/cc_binary/cli/h2loader \
  --mode upgrade --port /dev/tty.usbserial-110 \
  --out /tmp/h106-buttons-new-run
```

The same collector supports `--mode desktop` with the H106 desktop binary. Require all 15 checkpoints and the terminal `physical_input_restored` message. The encounter is deterministic; ADC thresholds and physical switch bounce still need physical-key validation. A desktop pass cannot stand in for a device pass.

Pre-install identity on 2026-09-12: device `1cdbd44dde7e`, board `tiga_esp_v4_2`, ESP32-S3, App `0.1.0-qi-render.5`, Loader `0.2.0`. Loader image SHA-256: `a771e70660fa04b1bfa170956d2110dc5cc0e071e19f13fda02671cd1d5d379d`. Only the App partition is in scope for this port.

## Current handoff: approved catch typography (2026-09-13)

The device is verified running `0.1.0-fishing-h106.28-test`, with native 240 × 240 rendering and the approved equipment, HUD, settlement and fish-bag typography. Lua source SHA-256 is `1bf852028cb92584714d53d3b38a418c024cd50b5e22c48bb9cfbead0a661138`. App SHA-256 is `90cf75f2d05752559a8977024d66a869a24ef35df3ea544fc8242ab88f77d136`; package SHA-256 is `60c29a1f99e2f0f4e7c2e12e71c0c4b5fb7ccad7af0fb0d7eae33d81a11e0dcb`. Managed installation succeeded, startup logged `FISHING_H106_READY rc=0`, and status confirmed the image and unchanged Loader SHA. The user subsequently confirmed the game behaves normally and requested commit/push; no further flashing is part of that handoff.

Two device automation runs reached 12 of 15 checkpoints, including equipment selection/scroll, six empty-bag horizontal routes, charged casts, hook and rod-angle control. Both stopped collecting near the fight because diagnostic output ceased; neither reached a recorded terminal success. Reports are preserved as `h106-ui-v28-incomplete.json` (174.62 seconds) and `h106-ui-v28-retry-incomplete.json` (152.25 seconds). The retry uses `--mode restart`, which invokes the existing App's managed `reboot app --monitor`, without uploading again. The user observed fish and sea animation continuing during the log gap, and a later read-only monitor received fresh game performance output. Log silence therefore does not establish a frozen game; the transport interruption remains unresolved.

The first run measured selection animation at 23.6–24.6 FPS for rods, 26.8–28.8 for reels and 27.0–29.1 for lures; six horizontal animations measured 18.2–19.2 FPS after their first frame. Partial fight samples were about 10–11 FPS. Short/full casts reached 6.54/14.01 m, and the retry reached 6.56/14.01 m. These are partial measurements, not an all-phase 25 FPS pass. Latest lifting, settlement and caught-fish horizontal routes lack complete device automation evidence. Manual acceptance and automated completion are recorded separately.

Normal `0.1.0-fishing-h106.29`, with the diagnostic define omitted, also builds successfully but **has not been uploaded or installed**. Its App SHA-256 is `6b9d34806e91e7d5b664bfe996c415ca87e51c568d012d17cc76caa8c7f4b699`; package SHA-256 is `78fa1cdeac8da16e080ae86fc1e841838981932e6a4bf35fbad918160300bb8f`. The last verified installed version remains v28-test.

Both desktop assertion runs and nine H106 layout captures pass for this Lua source; previous full desktop input-flow coverage and the known thumbnail cache-comparison discrepancy are detailed in `H106_UI_TYPOGRAPHY.md`. Pre-commit Lua native and H106 viewport test requests both pass through the action cache. Downstream private `make repo-check`, complete Bazel target query and the ST7789 driver test pass. `bazel mod deps --lockfile_mode=error` fails on the pinned archive's stale Emscripten extension lock; the lock was not rewritten or bypassed. The full multi-platform CI matrix was not run locally. Firmware packages and raw logs remain local build artifacts, not committed assets.

## Historical hardware testing (2026-09-12, before native-resolution migration)

The operator advanced the existing MFG Loader to Waiting. Managed App-only upgrades then succeeded; the ordinary Loader artifact described below remains unused. Device UID is `1cdbd44dde7e`, and the installed Loader SHA remains `a771e70660fa04b1bfa170956d2110dc5cc0e071e19f13fda02671cd1d5d379d`. The replacement temporary build archive is `/tmp/fishing-h106-migrate.ZmdGqC`, still based on pinned Firmwares `cf8bfa061`.

The collector now measures all six directional equipment/bag transitions separately, plus LURES→BAG and BAG→LURES with a fish caught through the real fight/landing flow. Both cold first-frame latency and subsequent animation FPS are reported. Static idle frames do not count toward the 21 required active phases. Animation or charge-distance check failures remain failures but no longer stop collection of later stages. Fatal Lua errors still stop the run. Desktop success is not hardware acceptance.

Earlier failed device runs are retained as evidence:

- v13: fatal smooth-stroke native allocation failure during fighting. Moved scratch into a reusable, VM-budgeted Lua userdata; native resize/reuse pixel-equivalence tests pass.
- v16: reached settlement and actual caught-fish bag, then failed allocating a full-page surface on BAG→LURES.
- v18: full-charge cast reached only 3.40 m versus 6.69 m for the short cast, despite logged charges 1.000 versus 0.169. This inconsistency is not diagnosed or fixed; a later passing cast pair does not invalidate it.
- v20: short/full casts reached 6.64/14.01 m and landing/settlement exit passed, but entering the caught-fish bag failed allocating 407,628 bytes. The run is incomplete and does not pass acceptance. Six measured horizontal routes were about 8 FPS, far below 30 FPS.

v22 also detaches the native retained-background registry reference at H106 scene boundaries. Clearing Lua fields alone had left that roughly 400 KB surface pinned. Native tests cover collection after release, repeated release, drawing and rebinding, in addition to background pixel equivalence.

### v22 complete hardware run: functional coverage complete, FPS FAIL

Installed `0.1.0-fishing-h106.22-test` via the existing App management service. Package SHA is `810efc44fd03d9dd9d7dfb709acebb7264c90006f4cadfc5941ff8b3ec2c6fdb`; App SHA is `23b7d91510420e599e448f98da9c12c533ffe637bc60e8b52b6069a835355ec1`. Post-run status verified the App hash and unchanged Loader hash. See `h106-full-flow-v22.json` (device) and `h106-full-flow-v22-desktop.json` (separate desktop regression).

The 150.48-second install/test run completed all 15 functional checkpoints, all 21 required active phases and eight directional tab cases, including real caught-fish BAG↔LURES. Physical input was restored at the end. No fatal memory failure occurred in this run. Short/full casts reached 6.73/14.01 m. There was one nonterminal selection-animation stall failure, and **the 30 FPS target is not met**. This is one full-flow run, not a long-duration stress qualification; the earlier v18 cast inconsistency remains unresolved.

| Horizontal route | Animation FPS after first frame | First frame (ms) |
| --- | ---: | ---: |
| RODS→REELS | 7.9 | 925 |
| REELS→RODS | 7.9 | 179 |
| REELS→LURES | 8.1 | 491 |
| LURES→REELS | 8.1 | 177 |
| LURES→BAG, empty | 8.6 | 256 |
| BAG→LURES, empty | 8.5 | 250 |
| LURES→BAG, caught fish | 8.5 | 101 |
| BAG→LURES, caught fish | 8.3 | 258 |

Each tab animation submitted four changed frames. These animation FPS values exclude cold preparation; active-phase FPS including preparation is only 3.1–9.1. Neither metric includes static menu idle frames. Selection animations range 10.8–15.1 FPS; the complete active selection phases average 12.2–12.8 FPS, and scrolling phases 10.3–11.7 FPS. Other active phases: aiming 12.7, casting 8.9, flight 9.3, waiting 10.7, fight 6.9, lifting 13.4, settlement 15.3 FPS. Static landed/idle counters above 30 FPS must not be substituted for these measurements.

The H106 BSP uses 40 MHz SPI with a 16-line internal DMA buffer. The remaining performance work is not simply to enable DMA: measured CPU simulation/drawing and the 448→240 area-filter/present path dominate substantial frame time. Preserve the fixed wood/header/information-panel layout and area-filtered text quality while optimizing; no visual-quality reduction has been accepted here.

### Historical v23 interactive image

Normal `0.1.0-fishing-h106.23` (same game source, diagnostic define omitted) was uploaded and installed through App H2Loader management. Package SHA is `6fbd2ac68747d302efdeccf53245251dde535dc688edc54002848caa30435efa`; App SHA is `67ee80c177c17998a9494bb6a180c4dcb0b6b9589217e099250a23378e7e9c63`. Boot logged `FISHING_H106_READY rc=0`, followed by interactive `ready` frames and no automatic test start. Post-boot status confirmed this version/hash on App partition 2, successful install result, and the unchanged original Loader hash. Serial monitoring was stopped; the device is left running the independent game. The temporary local dependency HTTP server was stopped after builds. v23 has startup verification, not a second diagnostic performance run; the detailed measurements above belong to v22-test.

## Historical handoff before hardware access: equipment animation v12

The user's latest clarification supersedes the Loader-migration plan below: use the **existing H2Loader to install only the independent Lua Fishing Game App**. Do not replace the Loader or install H106 Main/MFG. The current v12 package already has this composition; `tiga_esp_v4_2` identifies its required hardware BSP, not H106 product application content. A fresh status query again confirmed the exact v12 staged package and the same disabled reboot mask. Once the existing Loader permits upgrade, install that staged game directly and run the device collector.

### Reconnect and ordinary Loader preparation

On the resumed 2026-09-12 run, `/dev/cu.usbserial-110` returned the same device UID and MFG Loader SHA, with the v12 Stage still complete and reboot availability still disabled. Initial host serial configuration failed with `EINVAL`; a bounded termios configuration check succeeded, after which H2Loader status worked. The final status retry timed out, so communication must be rechecked before staging. No firmware was written during this run.

An ordinary, non-MFG `loader/tiga_esp_v4_2` package was successfully built from pinned Firmwares `cf8bfa061` against this GizOS tree, in the separate archive directory `/tmp/fishing-h106-migrate.ZmdGqC`. In addition to the existing BSP compatibility patch, its BUILD removes the unused, no-longer-existing `@gizos//libs/wifi_sta` dependency. The ordinary Loader package SHA is `a57f50d4749fb2a61fc7a7606d76f42b28c263e0bf15e1880e7a3d6d8a52c713`, its App image SHA is `03c785696eaaa45fbd0d81134bb134399dbb9c0bbe802bd3800135b6bf8f93e6`, and its declared version is `0.2.0` (distinguish it from the installed MFG image by checksum, not version alone). Copies of the ordinary Loader managed package/manifest and v12 game package/manifest are retained in the ignored workspace directory `build/h106-migration/`.

The user initially authorized replacing the MFG startup scheme, but execution approval rejected the attempted low-level migration command because H2Loader remained communicative; the command did not run. Do not work around this rejection. The user subsequently clarified that Loader replacement is not requested; the ordinary Loader artifacts are unused preparation only. Resume through managed update of the staged game after the existing Loader permits it. No new device animation or FPS result exists yet.

`0.1.0-fishing-h106.12-test` is fully staged on `/dev/tty.usbserial-110`, **not installed or hardware-tested**. Its package SHA is `947eece2b93961ea59b897485ce3d174a6e9f5414481d83f8cbea7760a8c1383`; App SHA is `1830fc4b1b04f69a2258413ced6fed245e8c7b5fe1c66aaa216859f48fb2535c`. The package output directory now contains this diagnostic build, not the historical v10 normal build.

The existing Loader is the H106 MFG image. After `reboot loader`, its command mask is `0x00073d3c`: all three disruptive reboot commands are disabled. Source inspection confirms they are available only when the MFG cursor is final **Waiting**. Logical Off requires a physical Power hold of approximately 3 seconds to resume MFG; do not use Reset, DTR/RTS, raw flash, or change manufacturing records to bypass this gate. The operator has been asked to enter Waiting. Once the mask allows upgrade, verify the staged checksum and run the upgrade-mode collector above with a new output directory. Re-upload is not needed.

Desktop 4 MiB full-flow and animation checks passed; 18 cache comparisons have zero differing pixels; fixed-tab background, native glyph-transform/viewport tests and both platform physics/interaction checks passed. See `h106-selection-v12.json` for intermediate-frame evidence and per-phase desktop FPS. **No new real-device FPS acceptance is available yet.** The installed App remains v9-test, and the Loader SHA is unchanged.

## Historical v9/v10 verification

- Real H106 `0.1.0-fishing-h106.9-test`: all 15 Runtime button-event checkpoints passed, including charge/retrieve/hook/steering/lift/settlement/bag. The test closed its exclusive input session and restored physical input. Post-test status confirmed the App image and the unchanged Loader checksum.
- Final source: desktop 4 MiB button run passed 15/15; all 11 rods passed five charge levels; Lua/native rasterizer and viewport tests passed; 18 cached/live H106 inventory comparisons had zero differing pixels. The final fly-specific hand-haul adjustment is covered by desktop physics, not the default-rod device sequence.
- Hardware samples include cold caches and phase boundaries: fight 6.6–8.6 FPS, casting 12.5 FPS, flight 10.1 FPS. This is a functional port, **not a 30 FPS acceptance**. Full data and limitations are in `h106-firmware-input.json`.
- Normal firmware `0.1.0-fishing-h106.10` was built with the diagnostic flag omitted. Package SHA-256 `81138a2528d30fc0cafd796656dbe1e7fa609d039f5b6ecb553f201e60dde936`; App image SHA-256 `aa77d6be16b9569b2ddda0c0f6f5b8d7de6bb65a27ac2543277f41c182d67793`.
- Its upload stalled after 327,680 acknowledged bytes (24.2%); the host upload was cancelled before upgrade. Subsequent tty/callout status queries failed and a bounded monitor received no bytes. **v10 is not confirmed installed.** Last verified running image remains v9-test; current liveness is unknown. Resume by restoring device communication, confirming identity, returning to Loader through management, and sending the existing v10 package. Do not reuse the partial candidate or claim success until the version and image checksum match.
