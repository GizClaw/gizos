# H106 migration validation

## Target and acceptance

The final target is the connected H106 Tiga V4.2 (`tiga_esp_v4_2`, ESP32-S3, 240×240), using its existing H2Loader and 8 MiB App partition. The user requires approximately 25 FPS across pairing, gameplay, skill close-ups, clashes and settlement, with no artwork reduction. Audio remains disabled. Runtime preparation and caches are approved; packaged artwork remains vector commands.

The private standalone entry is `projects/h106/targets/h2loader_tar_zlib/qi_duel/tiga_esp_v4_2` in Firmwares. It consumes GizOS through `@gizos`. Right/Left map to the game's next/previous skill component IDs; Record starts/confirms and dismisses settlement. The power button remains separate, with the product's three-second hold and board power latch. The launcher keeps H2Loader management available after a game error or while USB powers a logical-off device. Physical power and button validation remains pending.

## Startup and memory fixes

H106 has no touchscreen. The Lua application now accepts an unavailable touch provider on this layout, while retaining touch on desktop previews that provide one. Native tests exercise a provider that returns `UNSUPPORTED` and still require ready, presented frames and effective button actions.

A later real-round AMOLED run superseded the earlier short stability samples: round six ended with `invalid Lua vector commands`, Lua memory 4,648,421 bytes and minimum free PSRAM 8,404 bytes. This was an application exit under memory pressure, despite an empty CPU coredump. Native raster failures now distinguish invalid input, allocation failure, interruption and workspace limits. The Lua wrapper collects garbage and retries an allocation failure once, after every native workspace has been released; incremental collection also runs between frames. Injected allocation failures verify cleanup, retry pixels and both contiguous/scattered outputs. These tests do not establish long-running device stability.

## Exact coverage reuse

H106 prepares a 512 KiB coverage payload arena, with 639,008 bytes host storage (630,808 bytes on ESP32-S3) including its bounded key table. It caches the original floating-point pixel coverage of solid-painted contours. Keys include path points/closure, matrix, stroke width/type and destination dimensions, with full equality checks after hashing. Color, opacity, clipping and source-over order are evaluated on every frame. Large paths bypass the cache; an incomplete render never publishes an entry. Gradient paints keep the existing path.

Ten scenes (walls, arena, full battle, charge, absorb, guard, wave, clash, win, lose) were captured through the real desktop Display PAL with the software renderer, each for 30 consecutive frames at 33 ms scene increments. All 300 cached/uncached frames match byte for byte: mean and maximum channel error are zero. The evidence is [coverage-cache.json](validation/h106-vector/coverage-cache.json). This compares against the existing software implementation and is not a physical-panel capture or a new comparison against CoreGraphics.

Native regression also replays fills, clipping, transforms, malformed streams, cache eviction and changing light envelopes with warm caches. Four targeted suites (`vector_sw_test`, `vector_sw_oom_test`, `lua_test`, `lua_qi_duel_test`) passed after the coverage change. The final Record-only settlement test and UBSan runs of both native suites also pass. ASan could not run: the macOS sanitizer runtime recursed into its initialization lock before `main`; the process was stopped and the startup stack was saved. This is an unexecuted check, not a sanitizer pass.

## Device state and remaining verification

The H106 was reconnected on `/dev/tty.usbserial-410` and its UID `1cdbd44dde7e` and installed `0.1.0-qi-render.2` image were confirmed over H2Loader. Restarted normal firmware measured roughly 7–9 FPS in pairing, 39–42 ms for display submission and multi-second battle draws. The earlier disconnected-port uncertainty is resolved.

Further native changes preserve the original eight-by-eight polygon coverage via scanline fill/stroke unions; byte RGBA source-over uses an exact integer path at full opacity; icon filtering uses the float FPU with the double expression retained near byte-rounding boundaries. Each change group was compared over eight scenes and 240 frames, all byte-identical. Polygon masks additionally pass an independent point-sampling oracle covering self-crossing/concave contours, repeated vertices and clipping. These are software-renderer comparisons, not physical-panel captures.

The H106 driver packs RGB565 pairs directly to identical RGB444 bytes. Its default DMA reservation remains 16 rows (5.625 KiB); only the Qi Duel standalone CMake target opts into 48 rows (16.875 KiB). Both configurations pass driver tests including row stride, odd widths and multiple transfer tiles. Probe 4 measured 32–34 ms display submission, but pairing still missed 25 FPS and tilt frames reached 791 ms.

Probe 3 exposed a temporary diagnostic formatting error (fractional Lua KiB supplied to `%d`), corrected in subsequent probes. Probe 4 exited with `H2_PAL_ERR_FULL` during the transition into battle. The application now retains a pending input event when the VM delivery queue is full and retries after the queue drains. It preserves releases and does not abort on temporary backpressure; regression with 24 duplicate DOWN events followed by UP/ACTION passes and produces one skill action. Hardware validation is ongoing with the real-round probe. See [rendering-investigation.json](validation/h106-vector/rendering-investigation.json). The 25 FPS target and sustained runtime stability remain **unaccepted**.

The private exact firmware target builds. Full compatible macOS graph verification encounters unrelated H106 Main/GizClaw and TapDoki API mismatches with the working GizOS override; the test graph reports 52 passing, 81 failing to build and one skipped test. Those failures are not suppressed or described as passing. Private ownership checks pass; module/lock verification and query also passed during target integration.

## Rendering checkpoint: September 11

The runtime coverage arena now stores exact nonzero row spans instead of full rectangular masks. Up to 32 KiB of the existing payload stages a contour before publication; no separate scratch allocation is added. Icon geometry and Gaussian shadows are compressed into the existing 512 KiB frame arena and reused across tint/sheen changes. Single-precision icon colorization uses the ESP32-S3 FPU, with the original double expression retained near byte boundaries. Artwork, sampling density and animation timing are unchanged by these native changes.

The latest colorization passes both 120 icon-style frames (four skill resources, focus/direction/palette/sheen/opacity variation) and 240 complete-scene frames, all byte-identical to the prior software renderer. Sparse coverage, polygon scan conversion, compositing and geometry reuse each have their own 240-frame comparisons. These overlapping suites must not be described as distinct gameplay duration. All four targeted native/application suites pass on the final source. Renderer hot-file builds use `--per_file_copt=libs/lua/src/modules/h2_lua_(canvas|vector_sw)[.]c@-O2`; quote this argument in a shell. No fast-math option is enabled.

Probe 7 reached 31 real rounds over approximately nine minutes without an observed application exit, but monitor gaps prevent a continuous stability claim. Minimum free PSRAM reached 8,432 bytes, so memory headroom remains a risk. Probe 8 reused icon geometry; probe 9 adds sparse coverage and the two-file compiler override. Warm probe 9 play frames were around 2.7 seconds, with cold selection frames sometimes above ten seconds. Frame-time samples exclude some diagnostic logging overhead and are not panel refresh measurements. The latest version and final sample are recorded in the linked investigation and firmware JSON files.

Next work should focus on cold icon geometry/cache eviction, per-frame action slice transforms, prepared-body affine composition and full-scene particle paths. Pairing, transitions, close-ups and settlement still need complete physical-device acceptance. Keep audio disabled, preserve Left/Right/Record controls and the independent power lifecycle, and do not treat this checkpoint as meeting the approximately 25 FPS requirement.
