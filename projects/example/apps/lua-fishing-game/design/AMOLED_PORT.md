# AMOLED fishing port — work in progress

## Target and installation

Target: `//projects/example/targets/h2loader_tar_zlib/lua-fishing-game/amoled:package`.
The launcher uses the shared portable app and embedded Lua source, the AMOLED
PSRAM allocator, FT3168 touch, BOOT back action, an 80 MHz display clock, and
H2Loader app confirmation/recovery commands. No textures or replacement artwork.

Connected device: `/dev/tty.usbmodem5101`, board `amoled`, target `esp32s3`,
UID `94a990281a12`. Before installation, loader partition was valid, stage empty,
and the existing app version was `retained-pair-v3`.
Install only through managed H2Loader send/reboot-upgrade/status. No raw flash.

## Retained pixel transport

Fishing opts into `display.present({retained=true})`. The display runtime keeps
an RGB565 copy of the previous successfully submitted frame in the runtime
allocator (PSRAM on this target). It compares 16×16 tiles and merges neighboring
changed tiles into horizontal runs, then joins vertically touching runs.
The complete change map is computed before any panel write, reducing the time
between the first and last updated region. Old geometry is erased through the normal
Lua redraw, then the final pixels are compared; unchanged pixels do not need
panel transmission. First initialization sends the complete initial frame.
Errors invalidate the comparison baseline so a retry restores the whole image.
Closing the display or releasing the job frees both buffers. Other apps retain
legacy behavior unless they explicitly opt in.

This changes panel transport, not art resolution, palettes, geometry or effects.
The extra comparison buffer is 329,728 bytes at 368×448 RGB565. Lua still draws
the sea scene each frame at this stage; measured CPU drawing and physics costs
are being optimized separately. `FISHING_PERF` reports transmitted pixels and region count.

## Verification gates still required

- Native retained transport tests and frame comparisons with desktop output.
- Package checksum, successful app confirmation, active identity and empty stage.
- Real board steady/cast/retrieve/fight/deck/menu frame timing and memory.
- Local refresh region statistics during movement and zero transfer when static.
- Device interaction, graphics fidelity and absence of watchdog/reset/leaks.
- At least 30 FPS animation on hardware without removing visual detail.

A successful build or desktop FPS does not establish these hardware gates.

## Measured progress (2026-09-11)

All measurements below are board serial telemetry, not desktop estimates.
The configured 60 FPS cap is a target, not an achieved rate.

| Firmware | Ready | Post-cast waiting | Main change |
| --- | --- | --- | --- |
| fishing-retained-v2 | 1.4–1.6 FPS | — | Tile-diff transport |
| fishing-geometry-v4 | 2.8–3.1 FPS | 1.3–1.4 FPS | Native polygon/ellipse scanlines |
| fishing-mesh-v5 | 4–4.4 FPS | — | Compiled reel geometry |
| fishing-physics-v6 | 4.2–4.6 FPS | — | Native XPBD constraints |
| fishing-native-v8 | 7–8 FPS | ~3 FPS | Batched rod strokes, cached HUD, native rod geometry |

v8 waiting physics costs about 230 ms/frame, drawing about 80 ms, and
changed-tile transmission about 12–14 ms. This is not acceptable performance.
The next candidate adds native water/air integration, relative/axial damping,
elastic rod stepping, and cached boat commands. Hardware validation is pending.

The app-owned `fishing_math` module uses the same calibrated equations as the
Lua reference. Graphics remain authored in Lua; native display primitives
accelerate rasterization without image assets. All 64 rod segments and original
reel faces remain. Desktop checks compare native and reference cast node
positions within 1e-6; 18 scene captures pass visual/layout and game-state checks.
Nine deterministic captures, including idle, gear tabs and deck landing, match
the previous native-rod output pixel for pixel after the newest kernels.

Touch polling retries transient FT3168 IO failures instead of exiting the app.
The HUD uses a 26 px top inset for the rounded AMOLED screen. Sustained hardware
30 FPS is still required; 60 FPS or higher is desirable if stable.

## Partial-update tearing investigation

The user observed horizontal trails during animation. The AMOLED driver sends
rectangles synchronously and `amoled_present()` is a no-op. No scan-synchronizing
TE GPIO wait is wired in this implementation. Mixed old/new scanout is therefore
a plausible cause, not yet confirmed by a stationary-versus-moving observation.
v10 batches difference calculation before writing and vertically merges dirty
runs to reduce transactions. It does not claim atomic panel presentation or
complete tearing elimination. Tests cover vertical joining, isolated changed
tiles, right/bottom edges, old-pixel erasure, and unchanged frames.

Native v9 improves waiting to roughly 3.0–3.3 FPS; its profile still shows
~200 ms physics, ~39 ms rod/reel drawing, and ~25 ms projected line drawing.
Further numerical/raster optimization is required; hardware performance remains
below the requested minimum. v10 was sent through H2Loader with SHA-256
`f32709bfadc6174ac00bd3d66734885a9f2cd4b84e3e1e6e9867881a5c8d78f5`.

## Current source checkpoint

Equipment tab labels are informational: horizontal drags switch pages; vertical
drags scroll the selected catalog. Header taps are explicitly regression-tested
to leave the active tab unchanged.

Additional optimizations preserve procedural Lua artwork: reusable rod coordinate
tables, cached reel scanline commands, precomputed per-substep constraint weights,
and a combined double-precision rope substep that exchanges node tables once.
The adaptive rasterizer uses float arithmetic only when its error bound cannot
change the integer pixel result; boundary cases retain the original double
expression. A 304-polygon randomized/boundary comparison and deterministic scene
comparisons pass. No full-single-precision solver is enabled: that experiment
produced ~0.097 m node drift and was rejected.

The user still observes horizontal stripes during movement. Batched dirty-region
submission has not established a fix. Hardware scanout/transfer diagnosis and
sustained 30+ FPS animation are open requirements, not completed features.
The source launcher is restored to interactive `idle` after diagnostic builds.

## Resume checkpoint

See [OPTIMIZATION_HANDOFF.md](OPTIMIZATION_HANDOFF.md) for the installed v15
identity, measured 10.8–12.2 FPS ready / 4.4–4.5 FPS long-line waiting, verified
GPIO13 TE mapping, rejected and promising numerical experiments, and exact
continuation steps. The hardware performance/stripe requirements remain open.
