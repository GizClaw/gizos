# Fishing optimization handoff — 2026-09-11

## Checkpoint and completion status

Branch: `lua-fishing-game`, remote: `origin` (`GizClaw/gizos`).
Worktree used here: `/Users/grezlu/haivivi/gizos-amoled-fishing`.
Do not overwrite the separate `/Users/grezlu/haivivi/gizos` checkout.

- `083f952c`: retained display updates and procedural raster acceleration.
- `2507f771`: current fishing game, equipment gestures and AMOLED target.
- This handoff records progress; it does **not** mark the AMOLED port complete.

The acceptance target remains sustained **30+ FPS during animation**, preferably
60 FPS or higher, with the existing artwork, physical behavior and usable touch
input. A static menu reporting 62.5 FPS is **not** evidence of animation FPS.
The user still sees many horizontal stripes whenever the screen changes.

## Implemented and checked

- Tab labels no longer switch equipment pages on tap. Horizontal drags switch
  RODS / REELS / LURES / BAG, including drags starting on the header. Vertical
  drags scroll the selected catalog. Regression checks cover header taps,
  horizontal dragging, short-drag snapback and vertical scrolling.
- Scene art remains Lua-authored geometry, with no imported textures.
- `display.present({retained=true})` compares against the preceding successfully
  presented RGB565 frame. It computes all dirty tiles before submitting writes,
  joining horizontal runs and vertically touching runs. First frame is full;
  subsequent unchanged frames transmit zero pixels. Errors invalidate the
  baseline so the next presentation repairs the complete screen.
- Native polygon/ellipse rasterization, compiled reel meshes, rod stroke paths,
  cached HUD/boat commands and a VM-local reel scanline cache reduce repeated
  work. Adaptive float raster arithmetic falls back to the original double
  expression near integer pixel boundaries.
- Rod coordinate tables are reused. Rigid rod sections share trigonometry;
  material weights are computed once per solver substep. `advance_rope` combines
  integration, constraints and damping into one double-precision native call,
  exchanging Lua node tables once per substep.
- Transient FT3168 read errors are retried instead of terminating the app.
- HUD top inset is 26 pixels to avoid the rounded AMOLED corners.
- Desktop build, Lua runtime tests, 18 scene checks, 304 polygon reference cases,
  retained-update boundary/erasure checks and native/Lua cast comparisons pass.
  Nine deterministic scene captures match the preceding reference pixel-for-pixel.
- ESP32-S3 package `fishing-gesture-v15` builds successfully and starts in normal
  interactive `idle`, not the automatic casting diagnostic scene.

## Actual measurements and limitations

These are serial measurements on the real 368×448 AMOLED, CPU 240 MHz, PSRAM
120 MHz, QSPI display clock 80 MHz. Earlier versions are useful baselines only.

| Version | Observed performance | Meaning |
| --- | --- | --- |
| retained-v2 | Ready 1.4–1.6 FPS | Initial retained transport |
| geometry-v4 | Ready 2.8–3.1; waiting 1.3–1.4 FPS | Native scanlines |
| mesh-v5 / physics-v6 | Ready about 4–4.6 FPS | Mesh and constraint kernels |
| native-v8 | Ready 7–8; waiting about 3 FPS | Rod pose/path and HUD work |
| batch-v10 | Waiting about 3 FPS; ~200 ms simulation | Dirty-write batching alone is insufficient |
| profile-v11 | Ready: rod pose ~21 ms across substeps; solve ~15 ms; rod stroke ~19 ms; mesh ~19 ms | Function-level hotspot evidence |
| reel-cache-v12 | Mesh cache hits 0–1 ms versus ~20 ms misses; short-cast waiting 6–7 FPS in sampled intervals | Different casts/node counts are not directly comparable |
| gesture-v15 | Ready 10.8–12.2 FPS; 51-node waiting 4.4–4.5 FPS | Latest installed checkpoint; 13.39 m cast / 11.90 m paid line |

In v10/v11, changed-region transfer usually cost 10–19 ms. GC sometimes added
100+ ms spikes. `FISHING_PERF`, `FISHING_DRAW` and `FISHING_KERNEL` logs distinguish
simulation, scene drawing, transfer and native-kernel time. Instrumentation is
still enabled on the AMOLED profile and should eventually become optional.

## Horizontal stripes: priority investigation

Current hypothesis is mixed-frame scanout, but it is **not confirmed or fixed**.
The user says stripes remain numerous on any movement after dirty-run merging.
Also examine QSPI timing and DMA lifetime rather than assuming all artifacts are
tearing.

Authoritative hardware reference:
[Waveshare V1 schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf).
It maps **LCD_TE to GPIO13**. The connected board uses SH8601 and FT3168, matching
V1; the vendor's newer V2 has different controllers.

`boards/amoled/esp32s3/src/h2_esp_board_display.c` currently:

- Enables the panel TE command (`0x35`) but does not configure/read GPIO13 or wait
  for its signal.
- Sends each rectangle immediately, splitting it into DMA chunks and draining
  the queue after each chunk.
- Implements `amoled_present()` as an open-state check, not an atomic swap.

Suggested next experiment: verify the TE pulse and polarity on GPIO13, then add
opt-in synchronization **once per frame before the first rectangle**, not once
per tile. Keep other applications' defaults unchanged. Measure write-window
length against the panel scan interval. Independently compare a conservative
40 MHz QSPI clock with 80 MHz if stripes persist; do not claim success without
looking at the actual moving screen. Never substitute continuous full-screen
refresh for the retained-update requirement.

## Numerical experiments worth preserving

A full float constraint solver was rejected: one desktop cast produced
**0.097148120 m maximum node drift** and **0.014675134 m range drift**. It was
removed and is not enabled in the committed game.

A separate mixed-precision probe kept world positions, span constraints and
state in double, using float only for short-span correction arithmetic. One
cast measured **0.000000267 m maximum node drift** and **0.000000497 m range
drift**. This promising probe was removed from the checkpoint because coverage
is not yet sufficient. Revalidate fly casts, long lines, sinking/surface/float
rigs, retrieval, heavy fish and tension before enabling it. Do not replace the
full solver with single precision or relax the reference tests to hide drift.
Local experiment helper: `/tmp/fishing-mixed-probe.py`; its local logs are under
`/tmp/fishing-mixed-visuals`. Temporary files are conveniences, not durable sources.

## Continue in this order

1. Resolve visible horizontal stripes using TE/transfer evidence.
2. Check the new combined substep and adaptive rasterizer on the real device
   through casts and retrieval, recording node count and cache hits.
3. Evaluate the mixed-precision *correction* kernel against the full reference.
   Preserve double world coordinates and endpoint constraints.
4. Measure gear-tab dragging and catalog scrolling while they actually move.
   The menu currently avoids work when static, but moving menus still rebuild
   geometry. Consider translated procedural command caches if this is a hotspot.
5. Inspect touch sampling independently of slow rendering; transient read errors
   currently cancel the pending gesture. Test quick drags as well as slow ones.
6. Run sustained casting, retrieval, fighting, landing, rain and menu interaction;
   verify p95 frame intervals, memory stability, correct erasure and absence of
   watchdog/reset. Only then claim the hardware frame-rate requirement complete.

## Build and verification commands

Run from the fishing worktree. Desktop:

```sh
bazel build //projects/example/targets/cc_binary/lua-fishing-game:example-lua-fishing-game
bazel test //libs/lua:lua_test
SDL_VIDEODRIVER=dummy python3 projects/example/apps/lua-fishing-game/tools/verify_desktop.py \
  --binary bazel-bin/projects/example/targets/cc_binary/lua-fishing-game/example-lua-fishing-game \
  --out /tmp/fishing-new-check
```

The verifier requires Pillow and a new output directory. On this machine the
bundled Python is under `~/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3`.

Firmware:

```sh
source /Users/grezlu/haivivi/firmware-devenv/export.sh
bazel build --config=esp32s3 --//tools/bazel:firmware_version=fishing-next \
  //projects/example/targets/h2loader_tar_zlib/lua-fishing-game/amoled:package
```

Use managed H2Loader only (`send --file ...`, `reboot upgrade --monitor`,
`status`). Port here was `/dev/tty.usbmodem5101`; re-enumerate it after moving the
board. Stop any serial monitor before issuing a new command. Do not raw-flash,
erase partitions or replace the valid loader. The CLI used on this host is
`/Users/grezlu/haivivi/gizos/bazel-bin/projects/h2loader/targets/cc_binary/cli/h2loader`.

Latest package SHA-256:
`f9b301e5d3b8d7efee484ea8054458211c26789564e0b52b899af116579d36d3`.

## Verified device state at handoff

H2Loader status confirms `active_version=fishing-gesture-v15`, app partition 2,
`stage_valid=0`, `last_result=0`, and a valid recovery loader in partition 1.
The active app image SHA-256 is
`7031240c68553e8226261c464734ea9becc93e30eb1a2f25c72e4cb75c52b3d4`.
The serial monitor was stopped and the port released after checking status.

v15 representative waiting frame: simulation ~150 ms, drawing ~52 ms, transfer
~12–15 ms, p95 frame interval ~256–258 ms. Native `advance_rope` accounts for
~125–129 ms, total rod-pose calls ~17–20 ms, stroke paths ~13–14 ms, and reel
cache hits ~0–1 ms. Occasional GC-related spikes remain. This points to the
constraint substep as the next performance target, with TE synchronization the
first visual-correctness target.

The local logs used for this checkpoint are `/tmp/fishing-hardware-v15.log`,
`/tmp/fishing-v15-status.log`, `/tmp/fishing-precommit-check.log`, and
`/tmp/fishing-precommit-lua-test.log`. The observations and hashes above are
recorded here so continuation does not depend on temporary files surviving.
