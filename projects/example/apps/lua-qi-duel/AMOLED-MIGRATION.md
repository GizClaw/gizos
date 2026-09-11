# AMOLED migration status

The final hardware target changed to H106 on 2026-09-11; see [H106 migration validation](H106-MIGRATION.md). The measurements below are historical AMOLED evidence. A later continuous battle ended with a Lua application exit under memory pressure, so the earlier short no-exit samples are not whole-game stability acceptance.

The selected architecture is Lua gameplay, vector/path artwork, analytic scene lights and the reviewed streaming retro synthesizer. The current macOS desktop implementation and acceptance evidence are documented in [VECTOR-DESKTOP.md](VECTOR-DESKTOP.md). CoreGraphics remains the accepted desktop backend. A portable software backend and a private Zero ESP32-S3 standalone entry are described in [ZERO-STANDALONE.md](ZERO-STANDALONE.md); device performance and stability acceptance remain pending.

## Shared frame reuse and partial refresh (2026-09-11)

The display compositor now compares final RGB565 pixels and uploads the union of
changed bounds. Unchanged frames do not upload. Battle lights and foreground
share one composition, avoiding false damage caused by converting the background
before drawing the actors. Retained RGB888 surfaces track occupied/edited 16x16
tiles so pairing fades and conversion skip black tiles. Cross-tile boundaries,
clipped edge tiles, old-pixel erasure and no-op presents have native regression tests.

Vector slices use exact resource/frame/transform keys in a lossless RAM cache.
The fixed 512 KiB arena accepts large frames through 4 KiB streaming reservations;
wrap/eviction does not require a second arena. Frame interpolation never mutates
a cache entry. FPU blending retains the original double calculation near rounding
boundaries. All 144 sampled battle/skill/clash/result frames are pixel-identical
to the preceding composition change. Combining the battle compositions itself
has worst full-frame MAE 0.044914/255, maximum channel difference 9/255, owing to
removal of the intermediate RGB565 quantization.

Top-down pairing now retains/fades old trails and draws only newly travelled
segments. Particle count, paths and 8x8 coverage are retained. Head arrays and
colors are reused; unused perspective/tail calculations are skipped. Both tile
tracking and head-only computation each match their preceding implementation on
360 frames exactly. History trails themselves are an approximation of the original
geometric tails: across both screen sizes and animation steps of 7/33/99 ms,
visible-region MAE remains below 30/255, but some bright-core means exceed 30/255.
These are software-frame comparisons, not physical-panel captures; do not describe
the history effect as pixel-identical to the old geometric trail.

`retained-pair-v1` measured typical pairing draw time of 119 ms, versus the silent
baseline's median 838 ms, with typical rate around 7 FPS and occasional slower
frames. Its 22-case diagnostic finished all selected skills/clashes/results with
no script exit/watchdog or stored coredump; PSRAM minimum free was 455,396 bytes,
internal minimum free 60,335 bytes. Frozen repeated-frame timings demonstrate cache
reuse but are not animation FPS measurements. Audio remains disabled.

The second diagnostic finished all selected phases without a script exit or
watchdog; coredump remained empty, PSRAM minimum free was 422,128 bytes and
internal minimum free 59,855 bytes. Warm frozen-frame timings were carousel
77 ms, charge cells 65 ms, HUD 90 ms, clash 502 ms and settlement 669 ms. Full
battle remained 4,289 ms, most skill poses about 3.8 seconds, and wave-hold
18,739 ms. These do not pass the frame-rate requirement. The final normal
`retained-pair-v3` also skips decoding original vector commands on cache hits;
144 software frames remain bit-identical after that change. The diagnostic
timings above are from v2, not measurements of v3 animation.

Normal `retained-pair-v3` was installed on `94a990281a12` at `/dev/tty.usbmodem5101`; H2Loader image/package checksums match the saved artifacts. The application is 2,889,568 bytes and update package 2,040,271 bytes. Audio is still disabled, with no audio output opened. Body preparation took 12,691 ms. The last 20 pairing telemetry windows measured median draw 85 ms and 8.9–9.8 FPS. Normal pairing PSRAM free/minimum was 3,466,868/3,129,256 bytes; internal free/minimum was 76,863/59,863 bytes. Coredump is empty. These normal pairing minima must not replace the much heavier diagnostic minima above. Temporary scene traversal is absent from the normal source; the serial monitor has been released. Artifacts/logs are in `build/qi-duel-amoled-retained-pair-v3/`. See [retained-rendering.json](validation/amoled-vector/retained-rendering.json).

Dynamic light paths,
transformed bodies/icons and tilted particle trails still require rendering work;
cold frame generation or eviction can still stall. **This is not 30 FPS or
full-game stability acceptance.** Touch polling error `-4` remains open.

## Display-only pairing baseline (2026-09-11)

Per the user's request, gameplay audio is disabled: neither the synthesizer nor
its audio outputs start. At this baseline the connected board `94a990281a12` ran
`silent-pair-v1`, verified by H2Loader checksums. Particle geometry also precomputes
immutable hashes/angles/trigonometry and skips unused perspective projections
while the camera is top-down. Across 2,910 sampled frames (six camera tilts), all
68,278,500 particle draw-command values match the preceding implementation exactly.
The native renderer, particle count, paths and sampling quality are unchanged.

Device audio telemetry is consistently zero. Pairing still takes roughly
800 ms to draw a typical frame, with occasional slower frames; typical measured
rate is 1.1–1.2 FPS. This is a display-only baseline, **not performance acceptance**.
The short sample has no script exit/watchdog and an empty coredump; touch error
`-4` persists. Artifacts/logs: `build/qi-duel-amoled-silent-pair-v1/`.
See [silent-pairing.json](validation/amoled-vector/silent-pairing.json).

## Drawing and runtime-cache work (2026-09-11)

The first software-raster optimization stage (`draw-opt-v1`) passed four native
Lua/game/audio suites and 144 software-desktop frame comparisons (worst mean
absolute difference 0.00762/255). Active-edge scanning, exact sample rejection,
particle batching and single-buffer unblended frames reduce work without changing
sample counts, geometry or timing. The scheduler now avoids yielding across
non-yieldable C callbacks, and long native renders cooperatively poll the RTOS.
The automatic probe completed battle, clash and settlement without the earlier
carousel exit. However, battle took 18.9 seconds, clash 17.9 seconds and warm
settlement 7.3 seconds per frame. This does **not** pass performance acceptance.
See [drawing-optimization.json](validation/amoled-vector/drawing-optimization.json).

The user approved a preparation screen and resolution-specific runtime pixel
caches. This prototype prepares the idle opponent and both hands at more
than twice their intended screen size. A job-owned 1.5 MiB budget bounds these
caches; they survive scene-cache resets. Packaged artwork remains vector data.
The preparation clock is separate from gameplay, and cached transforms preserve
the original destination-space arm fade. Unsupported enlargement falls back to
vector rendering. Other animation keyframes and procedural lighting still use
the software renderer; this first cache stage is not a complete 30 FPS solution.


The cache stores losslessly compressed generated RGBA in RAM (271,629 bytes on
AMOLED, 113,735 bytes on the H106 host). Preparation currently takes about 12.5
seconds on the AMOLED board. Four subpixel samples per destination pixel preserve
filtered edges; across 144 software-desktop frames, worst full-frame MAE is
0.590/255 and worst changed-pixel-only MAE is 6.506/255. These are comparisons
against the previous software backend, not captured physical-panel output.

Memory validation exposed allocation failures in the audio-enabled diagnostic.
The implementation now reuses primary frame scratch, divides the second crossfade
frame into at most 16 KiB row slabs, and stores contour edges in 32 KiB blocks
merged with a small heap. Active/crossing buffers follow simultaneous scanline
intersections rather than all path edges. These storage changes preserve exact
pixels on 170 component frames and pass independent ASan/UBSan checks. The audio-enabled `cache-audio-v7` diagnostic completed two selected poses for
each of four skills, equal/combo clashes, and loss/win settlements without a
script exit or watchdog warning. PSRAM minimum free was 170,796 bytes and
internal minimum free was 47,795 bytes; the coredump partition was empty. This
short probe does not establish full-game or long-term stability. See [runtime-cache.json](validation/amoled-vector/runtime-cache.json).


A wave-hold watchdog trace identified the native polygon shadow renderer. It now
cooperatively yields at row boundaries, retaining all 8x8 samples and Gaussian
calculations. The fixed probe measures warm battle at 4.60 seconds, skills at
11.97–28.73 seconds, clashes at 18.3–18.5 seconds, and warm settlement at 7.47
seconds per frame. The panel transfer remains about 25 ms. Touch polling error
`-4` still occurs. **Performance, touch reliability and complete real-device
animation acceptance remain open.**


At that checkpoint the replacement board `94a990281a12` ran normal `cache-opt-v6`, verified by
H2Loader image and package checksums. Its update package is 2,037,042 bytes and
application image 2,881,776 bytes; partitions remain unchanged. Normal preparation
takes 12,739 ms, then pairing and real BGM start. The observed pairing rate is
0.5–0.9 FPS, with no script exit/watchdog during the short startup sample and an
empty coredump partition. Normal pairing PSRAM free/minimum is
2,421,944/1,358,508 bytes; this is not the heavier diagnostic's peak usage.
Artifacts and captured logs are under `build/qi-duel-amoled-cache-opt-v6/`.
Temporary automatic scene traversal is absent from the installed normal source.

## Device performance investigation (2026-09-11)

The device at `/dev/tty.usbmodem5101` (`30eda0ae0bf2`) was running the user's
`size-opt-v2` keyframe build. H2Loader confirms successful staging/installation
within the existing download partition. Partition and SD layouts were not changed.

The slowdown is primarily rendering CPU cost, not an audio/display lock conflict.
With audio disabled, pairing still took about 8 seconds per frame. About 7.5 seconds
was spent in roughly 2,700 native particle strokes. Counting the same 8x8 subpixel
samples by scanline reduces strokes to about 1.6 seconds and pairing drawing to
about 1.9–2.1 seconds without reducing particle count. 2,000 randomized old/new
comparisons have a maximum channel error of 1/255. Panel transfer is about 25 ms;
previous early-return render branches incorrectly included it in `draw_ms`.

Runtime memory changes follow Flappy Bird's PSRAM-backed Runtime and reuse of
opened outputs. Qi Duel now creates effect tracks only on first use and keeps a
compact schedule instead of expanding every future note. The longest score's
host Lua schedule falls from 161,956 to 30,348 bytes; complete reviewed PCM
fingerprints remain identical. The AMOLED mixer and PCM queue allocator now uses
PSRAM, while I2S retains its own DMA-capable allocation and FreeRTOS queue control
blocks remain in internal RAM. With real BGM enabled, `perf-memory-v2` measured
64,115 bytes of free internal RAM (minimum 47,799), compared with 9,691 (minimum
1,131) in the baseline. These are snapshots, not full-game peak-memory acceptance.

**Performance/stability acceptance still fails.** Pairing is approximately 0.4 FPS.
Audio synthesis/pumping costs another 0.1–0.4 seconds per iteration, and shares the
Lua frame loop. An automated audio-off arena probe did not complete its first
frame for at least 179 seconds and repeatedly triggered CPU1 idle-task watchdog
warnings while `$lua/worker` ran. The test was stopped through H2Loader; clash and
settlement were not reached. These scene measurements were taken on the first
board. The user subsequently reported possible drop damage and replaced it with
AMOLED `94a990281a12` on the same serial port. The old board's intermittent touch
IO error `-4` cannot therefore be attributed solely to firmware. The next
rendering audit must also cover native software-vector scratch allocations,
which currently bypass the Runtime allocator. No watchdog setting was relaxed.

The final source removes all temporary fixed-frame, silent and automatic scene
probes. It retains accurate `audio_ms` and `present_ms` telemetry. Results and
limitations are recorded in
[performance-investigation.json](validation/amoled-vector/performance-investigation.json).
At that historical checkpoint, replacement board (`94a990281a12`) ran `perf-memory-v3`, verified by
H2Loader image/package checksums. Its 2,020,116-byte package installed successfully
with 77,036 bytes of nominal download-partition margin. With BGM enabled it reports
64,295 bytes of free internal RAM (minimum 47,391) and 3,578,496 bytes of free PSRAM
(minimum 2,160,524). Normal pairing remains about 0.4 FPS and touch IO error `-4`
also reproduces on this board. Its short pairing sample has no observed watchdog
or reboot and an empty coredump partition; this does not establish battle or
long-term stability. No crash is being attributed to the damaged screen alone.

Final artifacts are saved under `build/qi-duel-amoled-perf-memory-v3/`; this target remains
`no-release` until the outstanding performance and stability work passes.

## Historical device findings

### Pre-install vector build (2026-09-10)

At this checkpoint the device at `/dev/tty.usbmodem4101` answered H2Loader with
`board=amoled`, `target=esp32s3`, and `device_uid=30eda0ae0bf2`. Its checksum
matched the historical particle diagnostic below; vector installation was pending.

The `0.1.0-dev` pure-vector firmware builds successfully with
`--define=h2_qi_duel_software_vectors=true`. Its entry now starts normal pairing
and battle mode, with management-advertising pause/resume hooks. The target
requires that flag and is marked `no-release` pending performance acceptance.

| Capacity check | Before optimization | Delta-varint build | Keyframe build |
| --- | ---: | ---: | ---: |
| Application image | 5,514,624 bytes | 3,233,040 bytes | 2,852,768 bytes |
| Remaining space in 8 MiB app partition | 2,873,984 bytes | 5,155,568 bytes | 5,535,840 bytes |
| Compressed update package | 4,680,113 bytes | 2,398,768 bytes | 2,019,513 bytes |
| Margin in 2 MiB download partition | -2,582,961 bytes | -301,616 bytes | 77,639 bytes |

The keyframe build passes the raw 2,097,152-byte download-partition capacity
gate with 77,639 bytes (3.70%) remaining. A real LittleFS transfer is still
required to confirm filesystem overhead and the state of existing staged
content; the build has not been transferred to the device.

Optimization uses `--define=h2_qi_duel_screen=amoled` to exclude 16 H106-specific
clash/fade frames, and H2VG v2 opcode 14 for lossless coordinate deltas. Whole-pixel
polygons use unit 4; quarter-pixel polygons retain unit 1. Zigzag varints preserve
every coordinate exactly. Both backends bound varints to three bytes, validate
the original signed-16-bit coordinate range and reject truncated or noncanonical
input. Decoding adds no persistent buffers or allocations. The first optimization
shrinks the component bank from 3,272,053 to 990,518 bytes.

The keyframe optimization reduces the AMOLED bank again to 610,946 bytes. Each
four-frame action row retains two active poses and remaps its middle pose; the
idle row entry was never drawn. Charge cells retain one authored orientation
for empty, lit and five transition layers, and the existing affine transform
rotates that crystal into all five meter positions. The equal clash fades its
terminal keyframe with runtime opacity instead of carrying a separate four-frame
fade bank. All twelve main clash frames remain packaged: an endpoint-only
experiment failed the visual threshold and was rejected.

The original resource bank and SVGs remain the source reference. The build packer
round-trips every selected frame back to the original command bytes before
embedding. Rendering all 170 original component frames before/after encoding
produces byte-identical pixels in both CoreGraphics and the software backend;
see `validation/amoled-vector/packed-frame-comparison.json`. This comparison proves
encoding fidelity; it does not replace device performance or panel acceptance.

The current Loader stages the complete package at `/dl/update.tar.zlib`. AMOLED
maps `/dl` to internal LittleFS; its provider does not currently mount an SD
card. The source build now fits the nominal partition without an SD or partition
change, but device transfer and filesystem acceptance remain pending. No device,
partition, or SD writes were performed during this vector build.

The baseline package and native debug files are copied to
`build/qi-duel-amoled-vector/`, and the optimized build to
`build/qi-duel-amoled-vector-optimized/`. Exact hashes, package verification, and
the capacity gates are recorded in `validation/amoled-vector/build-report.json`
and `validation/amoled-vector/optimization-report.json`. Previous software-renderer host
comparisons do not establish AMOLED device frame rate or stability; both remain
unverified.

### Retired raster investigation

The investigation used `/dev/tty.usbmodem5101`, an ESP32-S3 AMOLED board reporting
16 MiB Flash, 8 MiB PSRAM and an 8 MiB application partition. The old unmodified
raster application exceeded image limits. Even a compact raster experiment reached
13,283,344 bytes and failed the application partition check. No partition changes
were made. Particle-only experimental firmware measured roughly 0.1–1 FPS.
These measurements concern retired implementations, not the current vectors.

Historical frame manifests, CRC comparisons and memory/coredump snapshots remain
in `validation/amoled/`. Detailed temporary logs were stored under
`/tmp/qi-duel-amoled-baseline/` and may expire. The last recorded hardware image was
a particle diagnostic; source cleanup does not change the firmware on the device.

## Removed experiments

The cleanup removes the particle-only boot/replay profile, renderer profiling,
optional capsule/tapered-path backend, offline mip format H2M8, derived-tint format
H2DT and their packers/tests. These paths were superseded by the no-texture
requirement and are no longer supported build modes. Recorded evidence is kept
for context; old flags and probe commands must not be used for new builds.

## Remaining implementation and validation

- Optimize the portable vector backend for ESP32, then measure decoded geometry and peak heap against real limits. The first software backend passes initial host image comparisons but has not met the 30 FPS requirement.
- Use the normal playable startup and capture device-submitted RGB565 frames at
  the same timestamps as desktop for pairing, all skills, close-ups and results.
- Compare every frame against the accepted desktop and check actual 30 FPS
  presentation deadlines separately from capture overhead.
- Exercise input, BLE and audio together; repeat rounds/restarts and inspect heap,
  stack margins, watchdogs and coredumps. Inspect the physical panel as well.

The original raster resources remain for visual reference and the existing
non-vector build path. Their retention does not select them for final firmware.
No flashing, SD writes or partition changes are part of this source cleanup.
