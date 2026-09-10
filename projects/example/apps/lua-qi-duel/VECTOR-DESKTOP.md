# Native desktop: accepted vector components

The user approved both standing hands and authorized autonomous acceptance of
remaining components below **30/255 RGB mean absolute error**. The replacement
now uses the accepted hand/character/icon paths, simplified color-region vector
contours for the other artwork, analytic Lua scene lights, and the reviewed
streaming retro synthesizer. The rejected bulk hand/word redesign was removed.

User-directed changes to the old layout and pose:

- The charge poses are restored to the original artwork. All three active absorb
  poses have straight wrists and horizontal, parallel arms, one above the other.
  This user-directed absorb pose change is recorded separately from the
  original-pose error gate.
- All four skill icons are scaled to 75% of their previous size on both screens;
  orbit centers and touch targets stay in place, exposing the charge ring.

## Run the bitmap-free desktop

```sh
bazel build -c opt //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel-h106 --define=h2_qi_duel_desktop_vectors=true --define=h2_qi_duel_vector_only=true
```

Run either resulting executable without arguments for the playable pairing and
battle flow, including audio. `--rehearsal` opens the visual arena directly.
No-argument rendering enables all accepted components.

The vector-only configuration does not link or register original bitmap/light
atlases or prerecorded PCM. The original graphic resources totaled 20,652,870
bytes; the final vector pack plus seven approved vector resources totals
3,424,308 bytes (about 83.4% less). These are **art resource bytes**, not complete
firmware or executable sizes. Lua application code, audio note data, fonts,
platform libraries, stacks and framebuffers are additional.

The other components' 3,272,053-byte pack contains zlib-compressed H2VG polygon
commands, not texture pixels. Settlement streaks use the original six geometric layers and a bounded Gaussian-layer approximation, avoiding dense contours of blurred pixels. The SVG exports are inspectable geometry.
`app/tools/vectorize_components.py` reproducibly derives the command pack and
Lua directory using 32 colors and 0.45-pixel contour simplification, including
the explicitly changed absorb pose. The old raster art is an offline reference
input only and is absent from the vector-only runtime.

## Acceptance evidence

`validation/components-final-keyframes/report.json` covers 70 component/screen
cases. `validation/components-final-animation/report.json` covers 48 sequences
of 61 frames at 33 ms intervals: **2,998 frame pairs** in total. The worst frame
outside the user-directed pose exception is **20.4105/255**, below the 30/255
limit. A further 244 frame pairs in `validation/components-optimized-animation/report.json` verify the optimized victory/defeat streaks; their worst frame is 7.39/255. Each frame uses the union of component pixels above 3/255, with an
explicit countdown ROI; shared empty background is excluded. Every individual
frame must pass, not merely the sequence average. Poses, fades, health effects,
charge effects, clashes and both settlements are included on both resolutions.

`validation/components-absorb-correction/report.json` supersedes the player charge
and absorb sequences with 244 fresh frame pairs. Restored charge poses pass at
a worst-frame error of 10.74/255; the requested absorb pose change is recorded
separately. A geometry audit confirms that only these six active pose frames changed.

Open `http://127.0.0.1:4189/desktop-preview/component-acceptance.html` for native
frame playback, original/vector/overlay views and per-frame measurements.
Standing-hand approvals remain archived in their individual review pages.

To create fresh reference comparisons, build without
`--define=h2_qi_duel_vector_only=true`, then use
`app/tools/compare_components.py --output <fresh-directory> --frames=61 --sequence`.
`--draw-component=reference` requires that reference-resource build. Explicit
component switches isolate that component from other accepted changes.
Captures use the native Lua RGB565 framebuffer through SDL's dummy video driver,
so user interactions cannot change fixed-time probes. The review is not a
browser reimplementation. Capture reuse checks the build and vector-pack hashes.

## Runtime and memory checks

`app/tools/check_vector_runtime.py` exercises both screens with all components
active, including all four actions for both actors, equal/player/enemy beam clashes and both settlements. Twenty screen/scene probes pass at 29.2–30.2 FPS.
It retains performance logs and selected frames under
`validation/components-final-runtime`.

The vector path decoder has a 512 KiB upper bound and allocates only as needed.
Static geometry can reuse a resolution-specific render cache capped at 1 MiB
(and smaller on tiny canvases). These are transient RAM buffers created by
vector drawing, not shipped bitmap artwork. Animated large keyframes bypass
that cache. Frame interpolation uses premultiplied RGBA exactly as the previous
sprite operation did. Cache reuse, invalid slices, truncated compressed input,
polygon limits, affine transforms and drawing correctness are covered by tests.

```sh
bazel test -c opt --per_file_copt='.*test.*\.c@-UNDEBUG' //libs/lua:lua_test //libs/lua:vector_cg_test //projects/example/apps/lua-qi-duel/app:lua_qi_duel_test //projects/example/apps/lua-qi-duel/app:qi_duel_retro_audio_test //projects/example/apps/lua-qi-duel/app:qi_duel_rules_test --define=h2_qi_duel_desktop_vectors=true --define=h2_qi_duel_vector_only=true
```

All five test targets pass. Audio tests compare all 16 complete synthesized
waveforms with the approved review audio, as well as partial writes, looping,
termination and retained-memory bounds. Game tests cover both resolutions,
actions, health, carousel input, clashes and victory/defeat flows.

The current vector backend uses CoreGraphics on macOS. These desktop results
do not certify an ESP32 backend, physical display timing, device heap behavior
or H106/AMOLED firmware partition capacity; those need device-side validation.

## Source cleanup regression

The cleanup removes retired PCM audio, A4 assets/decoder, capsule and batched-path
experiments, H2M8/H2DT packers/decoders, particle-only firmware probes, unused
skeletal transforms and uncalled Lua drawing helpers. Desktop and reference
builds now share the reviewed retro synthesizer and the 14 MiB VM limit.

Twenty full-scene probes pass at 29.2–30.2 FPS. One hundred retained RGB frames
match the pre-cleanup desktop exactly; see `validation/cleanup/visual-regression.json`.
The initial run overlapped an old active desktop window; its performance results
are retained separately, and affected cases were rerun after closing that window.
The native capture tools reject unknown cases and return failure exit codes when
a required acceptance check fails. Original raster sources and their generators
remain available for visual comparison; obsolete candidate audio was removed.
