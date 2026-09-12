# 30 FPS optimization work in progress

Target: sustained real-device animation and equipment scrolling above 30 FPS.
Keep 40 MHz QSPI, retained updates, original artwork and physics regression
tolerances. Do not count static menu FPS as completion. No performance changes
in this work log have been committed yet. Navigation/Bags changes also remain
uncommitted and must be preserved.

## v19 experiment

- RGB565 procedural snapshots and clipped/keyed blits, bounded to one equipment
  page. Prewarm thumbnails on page entry; cache background/header and details.
- Skip definitely slack zero-lambda constraint spans, retaining double state.
- Temporary `perf-demo` firmware: automatic eight-second uncached/cached pairs
  for rods, reels, lures, then one cast and waiting. Restore `main.c` scene to
  `idle` for the final interactive firmware, not before benchmark builds.
- Device `/dev/tty.usbmodem1101`, UID `94a990281a12`, H2Loader at baud 115200.
- Installed v19 app SHA256
  `1ad98bb6df1ea7ac37e227d0018584d75e42d840c4615f72720e636d89edcb71`;
  package `8bd1927ce678e99b7729430d28adfb8ab3e526b4e854b0b9b3bb2d3a28f6f627`.
- Dynamic uncached menus ~2.8–4.5 FPS; cached ~12–14 FPS. Cached draw ~43 ms,
  present ~29–37 ms, versus uncached draw ~184–292 ms. Still below goal.
- Waiting after 13.39 m cast / 51 nodes ~4 FPS. Slack skipping alone is not a
  meaningful solver improvement. `advance_rope` still ~100–132 ms / sampled frame.
- Host snapshot API tests and all game regression checks passed. Eighteen
  cached/uncached equipment captures differed at 2–37 pixels per frame; all
  differences stay within a one-pixel raster-edge neighborhood, not byte-exact.
  Art geometry is unchanged, but prerasterized integer translations round a
  few edges differently. This caveat must not be hidden in visual verification.
- Capture comparisons:
  `/var/folders/q9/574_9l6d12b6z7lmvdgl33w40000gn/T/fishing-cache-ab-521kqtk2`.
  Original new-navigation captures: `/tmp/fishing-bags-navigation-v18`.

## v20 measurement

- Scroll-only repaint keeps header and footer untouched. Dirty tile comparisons
  skip tiles outside the tracked dirty bounds.
- Optional retained `merge_gap` (0 by default, 3 in inventory) trades a bounded
  number of clean tiles for fewer SPI command/rectangle transactions.
- `refined_sqrt`: hardware float seed + one double Newton refinement, with
  libm fallback outside normal magnitudes. All physics state stays double.
  Existing full AMOLED-profile game checks passed without relaxing the 1e-6
  native-vs-Lua cast/node tolerance (`/tmp/fishing-perf-v20-newton.log`).
- Further inventory timing instrumentation separates base/items/rail.
- Dynamic cached menus ~15–17 FPS (base 5, items 22–24, rail 6–7,
  present 21–30 ms). Waiting ~4 FPS. Build log `/tmp/fishing-perf-v20-build.log`.

## v21 measurement

- Masked snapshot row bounds, precomputed panel palettes, cached panel strips,
  and precompiled rail commands for integer scroll positions.
- Local constraint corrections use float but positions remain double; original
  1e-6 native/reference trajectory tolerance passes on both profiles.
- Package `141ee5f322299ed2d064ca02046324c06fa97dd66f709d0a769b9d2e6d12e46a`.
- Dynamic cached menus ~24–27 FPS: draw 14 ms, base 5, items 8, rail 0–1,
  present 22–30 ms. First page prewarm 1.1 s rods, 3.7 s reels, 2.8 s lures:
  this entry latency is still a problem, not a free performance improvement.
- Waiting ~4.2–4.8 FPS, advance_rope ~103–116 ms per frame.

## v22 measurement

- Optional present({retained=true,bounds=true}) sends one exact dirty bounding
  rectangle, avoiding tile comparisons and many small transactions for menus.
  It still updates the retained baseline, tested against subsequent tile mode.
- Native perspective segment projection / near-plane + screen clipping replaces
  transient per-line Lua tables. Constraint strain uses rationalized difference.
- Package `11364adaba76a79219f13b39231461dcdc473aba0f9d4e8eebb99b455c70b6c7`.
- Rods ~30 FPS, reels ~26–28, lures ~29. Not all above 30; actual changing frames
  still take 34–36 ms. Present ~20–21 ms for 105984 pixels / 1 region.
- Waiting ~5.2–5.8 FPS, sim ~118 ms (advance 94), draw 39–47 ms (line 11,
  stroke 14), present 12–15. Still nowhere near target for sea.

## v23 in progress

- Combine panel strips and wood in one list background, restore only row gaps.
- Precompute integer active edge ranges for scanline rasterization, preserving
  half-open double-reference rules. Native projection tests added.
- Cache 18 shape moments for the 64-segment rod tip: 165 bend/yaw/rail cases
  agree with full geometry within 1e-9 m, without reducing segment count.
- Compensated two-float scratch coordinates during constraints, with explicit
  product residual; double world positions/integration and original 120 Hz /
  6 iterations remain. Startup numeric self-test must pass on the actual S3.
- Full desktop and AMOLED physics regressions pass with original 1e-6 threshold.
- Device arithmetic self-test passed: 3.17e-15 error. Package
  `66310405ae73d0407127f4f9a1da8e52af40e70309f43b07f31de42103d25abd`.
- Rods ~32 FPS, reels 27–30 (GC stalls), lures 30–32. Waiting ~6–7 FPS;
  sim ~90 ms, advance ~80, rod_pose ~5 instead of 19. Still below sea target.
- `tools/verify_cache.py` now captures 18 A/B pairs; v23 all pass, same 2–37
  one-pixel edge differences as v19. Output `/tmp/fishing-perf-v23-cache-check`.

## v24 measurement

- Fishing native math library only uses -O3 without fast-math. Added per-frame
  accumulated microsecond timings for table read / integration / constraints /
  damping / writeback; runtime time API passed as module context, no global state.
- Package `c42c6c1656574beaf99067318c90d3ff81a520af86a3c6d8be2b381974af9154`.
- Waiting ~6–7 FPS; ~7.8 / 13.6 / 28.4 / 21.7 / 3.0 ms for the five native phases
  (12 physical substeps per slow frame). Rods 32, reels 27–30, lures 30–32 FPS.

## v25 in progress

- Algebraic displacement formulation cancels velocity /h and subsequent *h in
  integration and damping. Axial damping uses squared distance instead of sqrt.
  No timestep/iteration reduction; AMOLED 1e-6 trajectory regression passed.
- Lossless packed snapshot rows omit transparent margins, including correct
  opaque and different-mask replay. Unit tests and 18 A/B captures passed.
- At most one sprite variant for each of 3 equipment pages; rail caches reused
  across entries. Generational GC intended to reduce hard-limit collection spikes.
- Package `3138d2357ca1f3a4f5f147579653be3c4b3b16547b806df392c40105470585a3`.
- Warmed scrolling: rods 31.6–32.5, reels 30.2–30.7, lures 31.5–31.6 FPS.
  Waiting 7.4–8.1 FPS; native phases ~7.9 / 8.8 / 28.3 / 11.9 / 3.1 ms.
  First entry cache construction still costs 1.1–3.7 seconds.

## v26–v28 measurements

- v26: split the existing DMA buffer into two disjoint slots, preparing the
  next conversion while the prior transfer runs. No extra DMA allocation;
  ownership/drain/failure behavior covered by host mock tests. 40 MHz unchanged.
  Added native depth-gradient polyline projection. Menus ~34–36 FPS; waiting
  still ~7–8 FPS with many small regions. Package
  `78c5202c5e8cb8e73f2668250dab60e33276f7923bc42e1864a9ea4ee3468ce4`.
- v27: sparse restore of prior dynamic tiles from a procedural background,
  and skip untouched tile comparisons. Background lifetime pinned/tested.
  Waiting ~8–9 FPS; normal sea restore ~5–7 ms, present ~3–8 ms, but rebuilding
  a full snapshot every second caused GC spikes (Lua memory 3–4 MB).
  Package `dbcc65c4c56fa62ecdd482228efd62cb28e5b1baf1895c5a66e6a82c437a8316`.
- v28: preconvert weights, rest lengths, compliance and lambda for each solver
  substep instead of each constraint iteration. Solve ~23 instead of 28 ms
  across twelve substeps; waiting ~8–9 FPS, still below target.
  Package `13c3c1b9e780019a96b635d23a04933404b1197614f4ffa9608fbd1283ee8bf5`.
- v27 native/uncached cast captures at 6 seconds were byte-identical. Twenty
  desktop scene checks passed at each revision. No timestep/iteration reduction.

## v29–v30 in progress

- v29: compare actual background inputs rather than a one-second timestamp;
  merge small sea update regions with gap 2; speed-optimize Lua renderer/runtime
  without fast-math. Float local damping displacements retain double world state.
  Original 1e-6 cast regression passes on both profiles. An added two-second
  underwater native/Lua comparison also passes at 1e-6 on AMOLED.
- v29 upload was accepted, but software reboot stalled before loader startup.
  User reconnected; a raw serial read captured USB reset and loader application
  write. Subsequently status verified v29 active on partition 2, app SHA256
  `e3f510940558223619bc64e2ab6d5705b26502ffdd7df33c11377fd62d751dcf`, package
  `a0922fd2c28786d2cdf2dd127c686f4a1f241f8b9a716e72857f2e1bcb3a49a3`.
  Recovery partition remains unchanged. Waiting measured ~9–11 FPS, simulation
  ~50–60 ms, draw ~28 ms, present ~3–8 ms: still below target.
- v30: exact-coordinate rod stroke cache, invalidated by geometry/style/clip
  changes; degenerate/overflow fallback. Unit tests compare exact RGB565 output
  and mutations; twenty desktop scene checks pass (`/tmp/fishing-v30-captures`).
  Native depth-path gradient/water-crossing validation tests added and pass.
  Firmware is still the temporary perf-demo benchmark, NOT final interactive.
- Still required: live sea >=30 FPS, sustained menu measurements, entry/slide
  latency work, final normal idle build and device visual confirmation. No claim
  of overall completion; no new commit/push in this optimization turn.

## v30–v34 follow-up measurements

- v30 exact-coordinate stroke replay did not improve moving rods: coordinates
  change every frame. Fishing stopped using this option in v32. Cached equipment
  scrolling ~34–36 FPS, but entry prewarm still ~1.1–3.7 seconds.
- v31: persistent material coefficients with invalidation on node count, rest
  lengths, mass, density, stiffness and timestep changes; float local interior
  integration with double world positions. Clear waiting ~12–15 FPS. An added
  two-second underwater reference test failed at desktop 240 Hz, so desktop now
  retains the full-double kernels; the optimized AMOLED 120 Hz path passes the
  original 1e-6 tolerance. Material mutation comparisons also pass exactly.
- v32: conservative FPU stroke quads fall back to double near pixel boundaries;
  reuse opaque background storage to avoid allocating 333 KB every refresh.
  Five fixed-seed cast captures (0.5/1/1.5/3/6 seconds) are byte-identical to the
  reference render. Fast quad validation alone did not improve rod timing.
- v33 skips screen clipping calculations when both endpoints are already inside.
  Waiting ~14–15.7 FPS, draw ~27 ms, rod ~14–15 ms; rainy/heavier scenes ~9 FPS.
- v34 introduces residual-corrected FPU division, tested on 100,000 ratios and
  in the real S3 startup check (relative error 5.85e-8, coordinate-pair error
  3.17e-15). App boot confirmed `fishing-perf-v34`; package SHA256
  `6528eddf0b0e1b4ec86b1ad49568fa404a81b7bd3b3d2ce6de496e4dfda3b308`.
  Waiting still ~10–15 FPS: simulation ~30–50 ms, draw ~27–37 ms, rod ~15 ms.
  This is not a 30 FPS sea result.

## v35 in progress

- Rail prewarm stores 36 fixed ticks once plus only the nearby changing ticks
  for each scroll position. Exhaustive rail invariance check and 18 inventory
  A/B comparisons pass (same existing 2–37 one-pixel thumbnail edge differences).
- Keep validated integer stroke spans directly, instead of recalculating all
  intersections with the generic polygon renderer after fast-path validation.
- Add residual-corrected FPU square root on normal magnitudes; exceptional
  values retain libm. 100,000 numeric samples, 1,000 stroke raster comparisons,
  native/Lua AMOLED cast and underwater checks pass without relaxing tolerances.
- v35 package `0c47f2c33d72d17bc97abc1359de83cfe17f40b95336779fa0b653fe31148864`.
  Startup numeric check passed on S3, including sqrt relative error 5.45e-8.
  Cached scrolling ~35–37 FPS; Lua memory after all pages ~2.6–2.9 MB instead
  of ~3.1–3.4 MB. Rod drawing ~13 ms instead of ~15 ms. Clear waiting still
  ~13–14 FPS, rainy waiting ~8–9 FPS. Square-root replacement slightly worsened
  integration/solver timings; reverted in v36 (not a retained optimization).
  Five fixed-seed cast frame comparisons passed byte-for-byte.

## v36–v37 in progress

- v36 reuses width-independent double normals between rod stroke layers,
  invalidating on every coordinate/count change; reuses projected rope endpoints
  between adjacent spans and evaluates only the visible material's color.
  Add a conservative float bound to skip clearly slack spans before compensated
  arithmetic. Near the boundary the original compensated test still runs.
- Unit tests cover normals reuse, geometry/width/color/count changes and
  degenerate segments. Full AMOLED physics checks and 6-second rendered A/B
  comparison pass. Application package
  `ebcf0462c85dd691edd40937013da122448d09771a3c118f13beeb2bb440eb9e`.
- v37 changes only this target's Lua task policy: same 64 KB stack size, internal
  RAM rather than PSRAM, fixed CPU1. Management/BLE policies remain CPU0/PSRAM;
  unrelated tasks retain their original defaults. Target policy tests pass.
  Package `39bea8f5a435076fa1812b00eef717e2b14d3d006c3cc2f6108f421dd2b2f8a6`.
  Device confirmed internal stack/CPU1 and audio startup, but DMA fell from 64
  rows to 32 and internal heap was only 13,527 bytes free (largest 7,680 bytes;
  minimum 8,815 bytes). Clear waiting ~14–15 FPS, still below target. Internal
  stack is not worth the reduced memory margin and is reverted in v38/v39.
- 40 MHz QSPI, disabled TE, 120 Hz physics and 6 solver iterations remain.
  Still a temporary `perf-demo`, not final interactive firmware; no 30 FPS sea
  claim and no new commit/push.

## v39 installed interactive checkpoint

- Restore `idle` startup. Keep CPU1 affinity but restore the 64 KB PSRAM stack.
  Remove v36's extra slack bound, whose additional calculations did not produce
  a convincing solver improvement (v36 clear waiting ~12–14 FPS).
- v39 initializes fast-raster scratch only over a segment's actual scanlines,
  not all 448 viewport rows per segment. v38 built but was not installed.
- Host tests pass: Lua numeric/rendering/ownership, DMA pipeline, target task
  policy, 20 scene captures with desktop physics/navigation checks, and 18
  cached/uncached equipment comparisons. Captures: `/tmp/fishing-v39-scenes`
  and `/tmp/fishing-v39-cache`. Final AMOLED physics checks pass; five fixed-seed
  cast captures at 0.5/1/1.5/3/6 seconds are byte-identical to the reference
  renderer (`/tmp/fishing-final-v39-*-native.ppm`). Navigation contact sheet was
  visually inspected: equipment headers contain only rods/reels/lures, with bags
  a separate page. This is desktop image verification, not hardware visual QA.
- Overall sea target is NOT achieved. User was asked whether maintaining current
  physical behavior or allowing a modest simulation-quality tradeoff is preferred.
  No simulation timestep, iteration-count or node-budget reductions were made.
- Installed and confirmed through status on `/dev/tty.usbmodem1101`:
  version `fishing-perf-v39`, app SHA256
  `4c9c4bcacb1e0c6f4f1e2af5b27a97a2ece09b4c766f46bb5a846c3be191fc17`,
  package `57009458bd2d1864e137a708469b72063c47d4ebcfca93aadae19e9020fdbf24`.
  Running partition 2, stage cleared, last_result=0. Recovery SHA remains
  `5ad3716ee7272661a4fe97eee7d3dc3adaf997f61060a0a3be6e466552b49afb`.
- Ready/uncast animation measured ~21.7–23.2 FPS over ~30 seconds; sim ~6–10 ms,
  draw ~29–31 ms, present ~2–5 ms. This is not the 51-node waiting benchmark.
  No new v39 post-cast or menu hardware measurement yet. Prior cached-menu
  measurements ~34–37 FPS and prior post-cast waiting ~10–15 FPS remain separate.
- DMA is back to 64 rows/47,104 bytes. Internal heap free=46,799 bytes,
  largest=31,744, minimum=29,783 at startup. Numeric self-test, audio and app
  confirmation succeeded. Serial monitor was stopped; the interactive game
  continues running. Hardware visual/touch confirmation is still needed.
- Remaining: >=30 FPS sea and heavy weather/fight scenes, longer per-menu tests,
  first-entry prewarm and horizontal transition latency, visual QA, cleanup of
  unused experimental stroke replay API/instrumentation before final review.
  All performance/navigation changes are uncommitted; baseline remains a2c83e77.

## Build environment

Reuse `/Users/grez/haivivi/firmware-devenv/export.sh`. The component manager uses
the existing mirror at `http://127.0.0.1:8765/`; start a localhost-only Python
HTTP server rooted at `.tools/esp-idf/component-mirror` if it is not running.
Do not raw flash, erase partitions or replace the recovery loader. Use H2Loader
send, reboot upgrade, status; stop the monitor before the next command.
