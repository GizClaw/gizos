# Browser → native SDL migration

## Approval gate

Migrate one numbered browser layer, compare its real native output with the
browser, then wait for user approval. Do not start the next layer before that
approval. Existing full-scene native code is **not** evidence of fidelity.
User authorized batching 03–07 on 2026-09-08 and then approved that batch.
User approved 08 and requested 10–11 together, including charge effects.
User approved 10–11 and authorized all remaining elements together.
Current gate: final 09/12–15 and full-scene review; all elements are implemented.

Reference: `desktop-preview/index.html`, restored Git blob
`c4baa99c075bb3014a91da2f4b2d069dc632b703` (uniform health / octagonal charge cells).
Do not silently modify this reference to make a comparison pass.

| Layer | Browser element | Native migration / approval |
| --- | --- | --- |
| 01 | 侧墙随机纵向短灯 | Approved by user on 2026-09-08 |
| 02 | 竞技场电子灯线 | Approved by user on 2026-09-08 |
| 03 | 轮盘环绕微粒 | Approved by user on 2026-09-08 |
| 04 | 空间双路径粒子 | Approved by user on 2026-09-08 |
| 05 | 对手完整立绘 | Approved by user on 2026-09-08 |
| 06 | 我方左手 | Approved by user on 2026-09-08 |
| 07 | 我方右手 | Approved by user on 2026-09-08 |
| 08 | 顶部状态栏 | Approved by user on 2026-09-08 |
| 09 | 技能转盘完整合成 | Implemented/self-checked; awaiting user review |
| 10 | 技能转盘底盘 | Approved by user on 2026-09-08 |
| 11 | 弧形聚气格 | Approved by user on 2026-09-08 |
| 12 | 聚气技能图标 | Implemented/self-checked; awaiting user review |
| 13 | 发波技能图标 | Implemented/self-checked; awaiting user review |
| 14 | 吸收技能图标 | Implemented/self-checked; awaiting user review |
| 15 | 防御技能图标 | Implemented/self-checked; awaiting user review |

Final integration is implemented and awaits visual acceptance. No AMOLED
flashing, Bluetooth pairing or real combat rules in this migration step.

## Review 01

Run the **actual native target**, not a browser window:

```sh
bazel run //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel -- --layer=walls
```

It animates the same 72 static-position lights as the browser, with identical
deterministic depth, phase, rate, brightness threshold and power envelope.
SDL initialization is explicitly on the main thread (required on macOS).

Rasterization: `assets/generated/wall-lights.h2lf` contains 65 static gain samples
per lamp, preserving Canvas subpixel placement, color gradients and glow. It is
not a movie or a screenshot sequence of the scene. Lua computes each lamp's gain
every frame; `display.draw_light_atlas` decompresses adjacent samples, interpolates
and adds in RGB888, then resolves once into the existing RGB565 framebuffer.
Its scratch buffer is charged to the Lua VM budget and dies with the VM.

### Checks

- Built the SDL target and ran it in a macOS window.
- Native live self-test: 30.0 FPS over five one-second windows; wall draw around
  4–5 ms on this machine. This is **01 only**, not a claim about final-scene FPS.
- Both `//libs/lua:lua_test` and the game's `lua_qi_duel_test` passed.
- Captured actual PAL submissions at 0, 1875, 4000, 9000 and 17000 ms and visually
  inspected side-by-side pairs at the same 368×448 resolution.
- Light positions, sizes, hue, fade phase and composition agree visually.
  This is **not bit-exact RGB888 parity**: the current SDL display is RGB565, and
  gain interpolation introduces small additional rounding differences. Full-frame
  mean absolute channel error against browser RGB888 was 0.051–0.067 / 255.
  See `desktop-preview/comparison/walls-report.json`; its lit-channel metrics
  avoid hiding differences in the mostly-black background.

### Reproduce

1. From repository root, run `python3 projects/example/apps/lua-qi-duel/app/tools/wall_reference.py`.
2. Open `http://127.0.0.1:4174/bake` in Chrome. Wait for “Atlas and five reference
   frames saved”. This uses the original Canvas functions unchanged.
3. Build the target above. For each time `N` in `0 1875 4000 9000 17000`, run:

   ```sh
   bazel-bin/projects/example/targets/cc_binary/lua-qi-duel/example-lua-qi-duel --layer=walls --time-ms=N --capture=/tmp/qi-duel-native-walls-N.ppm
   ```

   Capture requires a new path (existing files are never overwritten).
4. Run `python3 projects/example/apps/lua-qi-duel/app/tools/compare_layers.py --captures /tmp`.
   Open `desktop-preview/comparison/index.html` through the local preview server.
   Generated PNG/PPM files are ignored in Git; generator code, atlas and metrics
   are retained for reproducibility.

## Review 02 — approved

```sh
bazel run //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel -- --layer=wheel
```

Use `--layer=arena` for **01+02 only**. `--layer=full` now uses all migrated
elements (see the final review below).

The arena renderer now contains the reference's dark projected disc and lip,
seven rings of sparse fixed-position segments, radial fragments, and 12
world-space projected rectangular lights. There are 118 atlas entries including
the base. The `.8` light gain, individual fade envelopes and faulty-contact
flicker are preserved. All geometry is extracted from the original drawing code;
this does not approximate circles with a screen-space ellipse.

The original Canvas pass uses butt line caps when isolated, but inherits square
caps from the preceding wall pass in a full scene. Both variants are preserved
as static gain atlases. Lua carries the original brightness formulas and seeded
rectangle timing. The reference HTML remains byte-identical.

Evidence from 2026-09-08:

- Built the real target; inspected its macOS SDL window.
- `H2_QI_DUEL_SELF_TEST result=PASS ... measured=30.1 ... samples=5` for live 02;
  typical draw time 3–4 ms on this machine. Final integrated FPS remains unproven.
- Re-ran `lua_test` and `lua_qi_duel_test`, both passed. App tests now cover fixed
  frames for walls, wheel and combined arena, plus isolation bounds and cleanup.
- Compared and visually inspected all five fixed frames for 02 and all five for
  01+02. Geometry, spacing, projection, colors and animation phase agree visually.
- 02 mean channel error versus browser RGB888: 0.135–0.155 / 255; lit-channel
  mean: 1.63–1.83 / 255. There remain RGB565 quantization / interpolation rounding
  differences, not bit-exact color equivalence. Full reports:
  `desktop-preview/comparison/wheel-report.json` and `arena-report.json`.
- The already-approved 01 frame at 1875 ms was recaptured and was byte-identical
  to the pre-02 native capture (`cmp` succeeded).

Reproduction: run `app/tools/wheel_reference.py`, open
`http://127.0.0.1:4175/bake`, and wait for the ten reference frames. Capture times
`0 1875 4000 9000 17000` with `--layer=wheel` and `--layer=arena` using the same
native `--time-ms` / `--capture` options as 01. Then run `compare_layers.py` with
`--layer wheel` or `--layer arena` and the appropriate capture prefix. Review at
`http://127.0.0.1:4173/desktop-preview/comparison/?layer=wheel`.

## Review 03–07 — approved

Run `--layer=scene7` for migrated 01–07 only. Individual modes are `dust`,
`particles`, `opponent`, `hand-left`, `hand-right`; `arena-dust` isolates 01–03.

Lua reproduces the original seeded dust and 104 two-route particles, including
quadratic wall bends, perspective width/length, additive tapered trails and
top-edge fade. The native RGB888 compositor adds subpixel round strokes, then
quantizes to the PAL RGB565 framebuffer. Intact opponent and faded hands are
lossless H2R8 resources exported from original Canvas art; Lua applies original
breathing/rotation/translation. Premultiplied RGBA mipmaps avoid downscale aliasing.

Verified 35 pairs at 0/1875/4000/9000/17000 ms, five per single/combined mode.
Full-frame RGB888 MAE for scene7: 0.731–0.734 / 255; lit-channel MAE 2.90–2.95.
Differences remain in RGB565 color, stroke AA and opponent mip filtering (not
pixel-exact equivalence). Hand equal-RGB565 lit-channel MAE is below 0.1 / 255.
Reports are retained alongside the comparison UI. Reproduce with
`app/tools/batch_reference.py` on port 4177, original `/bake`, native fixed-time
captures, and `compare_layers.py --layer=NAME --prefix=CAPTURE_PREFIX`.

Both Lua/app test targets pass, including new affine/compositor/resource validation,
fixed-frame determinism and per-layer bounds. Approved walls/wheel/arena captures
at 1875 ms remain byte-identical. Original reference blob is unchanged.
Actual native macOS self-test: PASS, 29.5 FPS across five startup windows;
subsequent windows approximately 30 FPS. VM usage about 10.6 MB with 12 MiB limit:
this is desktop fidelity, **not an AMOLED memory/performance validation**.

## Review 08 — approved

Run `--layer=hud` for 08 alone or `--layer=scene8` for 01–08 combined.
Original portrait/bevel/rail art, 190×(190×116/398) panel size, x=10/168 and
y=57/15 placement are preserved. Each panel has five identical 20×13 slanted
cells, gap 2, slant 3. `hud-states.h2r8` stores the twelve static 0–5 HP panel
states; it is not an animation or complete scene recording.

Clicks use the original pointer-down behavior: left half decreases, right half
increases; values clamp at 0–5, no effect restarts at a clamped boundary. Each
side has an independent effect timer. Lua animates the entire panel with its
cells, white/cyan/orange pulse, Gaussian glow and four expanding short rays:

- Damage: original 280 ms decaying horizontal shake, amplitude 2.2 px;
  changed-cell afterglow lasts 360 ms.
- Recovery: original 480 ms white flash/glow, plus a **new user-requested**
  220 ms lighter shake, amplitude 1.1 px. Original browser only flashes on
  recovery, so early recovery comparisons intentionally include a small offset.
- Affine crop support samples the static panel states. Dynamic polygon/blur and
  ray geometry are rendered at runtime, not baked animation frames.

Validation (2026-09-08):

- `lua_test` and `lua_qi_duel_test` pass. New tests cover crop bounds, polygon
  fill/alpha/source-over strokes, invalid inputs, four timed health transitions,
  and PAL pointer-down producing the exact same first frame as the effect probe.
- Compared 36 frames at 0/50/120/240/360/480 ms: HUD, combined scene, and both
  directions for both players. Static HUD RGB888 MAE 0.175 / 255; after equal
  RGB565 quantization, lit-channel MAE 0.069 / 255. Dynamic shadow/AA differ
  slightly from Canvas; recovery movement is intentionally added as above.
- The original HTML blob remains unchanged. In the export harness an explicit
  `willReadFrequently:false` prevents Chrome switching raster backends midway
  through repeated readbacks. GPU minification of the opponent in this reference
  can differ from the prior batch's CPU readback; 05 is unchanged, and the actual
  approved native `scene7` at 1875 ms remains byte-identical after adding 08.
- Actual native macOS `scene8` self-test: PASS, 29.3 FPS over five windows.
  Live meter interaction windows measured 27.8–29.3 FPS, steady windows ~30 FPS.
  Window-system stalls were also observed during UI inspection (present rather
  than drawing time); this is not a guarantee against OS scheduling pauses.
  No AMOLED flashing or device-FPS claim.

Reproduce using `app/tools/hud_reference.py`, Chrome `http://127.0.0.1:4178/bake`.
Capture via `--layer=hud --health-fx=player-down --time-ms=120 --capture=NEW.ppm`
(also `player-up`, `enemy-down`, `enemy-up`; omit effect for static HUD).
Use `compare_layers.py --times 0,50,120,240,360,480` and the corresponding layer
and prefix. Review page: `/desktop-preview/comparison/?layer=hud`.

## Review 10–11 — approved

Use `--layer=carousel-frame` (10), `--layer=charge-cells` (11),
`--layer=charge-base` (10+11), or `--layer=scene11` (01–08 + 10–11, **no 09/skills**).
The original bottom sprite is preserved at (-6,267), 380×152, opacity .9,
including transparent padding. There is no newly painted opaque backing panel.
Cells use the original orbit (184,694), radius 359, span .74, ten positions.
Both lit and empty shapes are the same 25×15 octagon with 4 px corner cuts.

`carousel-base.h2r8` and `charge-cells.h2r8` preserve static art: 20 lit/empty
cells plus the five separate fill/material/shine/outer/inner passes for each
cell. Lua drives continuous effect scale/opacity and six rays; the assets are
not pre-recorded animation frames. Separating passes is important because the
reference applies alpha before each source-over operation, not after flattening.

- Charge gain: 520 ms, scale 1 + .2 sin(pi p), opacity .72(1-p).
- Charge loss: 420 ms, scale 1 - .18p, opacity 1-p, over the now-empty slot.
- Six independently positioned neon rays expand with the original angle .18,
  inner 12+4p, outer inner+4+10p, blur 7+9 sin(pi p).
- Pointer-down on the charge band x=42..326/y=312..370: left of184 decreases,
  right increases, clamped to 0..10. A limit tap does not restart the effect.

Validation (2026-09-08):

- Built actual Lua/SDL target; live combined self-test PASS, 29.3 FPS (five
  windows). Static affine UI translation has a direct source-over fast path.
  All 36 fixed captures before/after this optimization were byte-identical.
- Both Lua/app tests pass. Covers cropped sprite opacity, neon stroke validation,
  layer bounds, gain/loss temporal changes, exact probe-vs-pointer-down frames,
  and repeated taps at 0/10 preserving the last valid state/effect.
- Compared and inspected 36 paired reference/native frames, six times each:
  0/65/130/260/390/520 ms, for the base, cells, base+cells, gain, loss and combined.
  Layer10 is pixel-exact after equal RGB565 quantization; static11 equal-RGB565
  lit-channel MAE ~0.027/255. Animated resampling and Gaussian ray glow retain
  small differences; do not claim bit-exact animation. Full reports retained.
- Approved scene8 at0 ms is byte-identical after migration; reference HTML blob
  is unchanged. Prior opponent GPU/CPU reference filtering caveat still applies
  to combined comparisons; the approved opponent implementation is not changed.
- Desktop VM limit is now 14 MiB for native source art and cached filtered layers;
  this is not an AMOLED memory budget or device performance certification.

Reproduce with `app/tools/charge_reference.py`, then Chrome
`http://127.0.0.1:4179/bake`. For an effect use native
`--layer=charge-cells --charge-fx=up --time-ms=130 --capture=NEW.ppm` (or down).
Use `compare_layers.py --times 0,65,130,260,390,520` with the appropriate layer
and capture prefix. Review `/desktop-preview/comparison/?layer=charge-base`.

## Final review — 09 and 12–15

Run the real native app, with no browser dependency:

```sh
bazel run //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel -- --layer=full
```

`carousel` isolates 09 including the approved 10–11. `skill-charge`,
`skill-wave`, `skill-absorb`, `skill-guard` isolate 12–15 at their real screen
coordinates. All rendering is Lua → native compositor → PAL RGB565 → SDL.
The restored browser source remains unchanged.

- Four original sprite-sheet icons, with no substitute vector/A4 redraw.
- Original orbit center (184,678), radius278, angular step .39; center lies below
  the screen. Three visible icons wrap through four skills in the original order.
- Continuous focus controls size52..88, opacity .34..1, authored gray artwork,
  white edge glow and brightness. Left icon fades left, right fades right;
  directions change at the original +/- .035 rad threshold.
- Drag offset is clamped +/-90 and follows the original .24 per-frame easing.
  On release, travel beyond +/-28 selects the adjacent skill; shorter travel
  does not. Native release coordinates also cover quick DOWN/UP gestures where
  no intermediate MOVE is delivered. Charge-band taps are consumed before drag.
- No opaque backing or selected breathing ring. Hands, particles, base, cells
  and icons are composed in the approved order. Existing health/charge effects
  are unchanged.

`skill-styles.h2rs` contains 408 transparent static focus styles (4 icons ×
3 fade directions ×34 focus samples, including the .5 glow threshold).
These are not scene or animation frames: Lua computes current angle, focus,
selection and timing live, and the compositor interpolates adjacent styles in
premultiplied RGBA. A six-entry bounded cache avoids per-frame decompression;
all decoded cache memory is charged to the VM. Source limit8MiB, VM limit14MiB;
AMOLED memory/performance requires a separate implementation audit.

Validation:

- Lua runtime and app tests pass: style identity, premultiplied interpolation,
  invalid indices/blend/header/ranges/zlib; fixed layers; all four selections;
  both wrap directions and exact +/-28 no-switch threshold, with/without MOVE;
  existing health/charge temporal and click/boundary tests.
- 55 reference/native pairs across 0/1875/4000/9000/17000 ms: four individual
  icons, four resting carousel selections, left/right drag offsets, full scene.
  Static center icons equal-RGB565 lit-channel MAE below .2/255; carousel
  resting ~.42/255, intermediate drags ~1/255. Interpolated styles and subpixel
  translation are close, not bit-exact Canvas filtering. Full-scene MAE against
  original RGB888 ~1.60–1.63/255, lit-channel ~3.33–3.39/255; prior opponent
  GPU/CPU minification and RGB565 caveats still apply.
- Cache optimization retained byte-identical native captures for all four icons,
  carousel, full scene (five times each) and left/right intermediate drags.
  Approved scene11 at0ms remains byte-identical to the pre-09 migration frame.
- Full native SDL self-test passed at29.4FPS across five startup windows;
  steady windows approximately28–30FPS, with OS/window-inspection stalls possible.
  This is desktop measurement, not a device-FPS guarantee.
- Real window and charge/health mouse clicks were inspected. The available GUI
  automation drag delivered identical native DOWN/UP coordinates (250→250),
  so it could not establish end-to-end manual drag acceptance. PAL event tests
  cover movement/selection and captured intermediate poses cover geometry;
  manual mouse swipe feel remains part of final user review.

Reproduce assets/reference with `app/tools/skill_reference.py` then Chrome
`http://127.0.0.1:4180/bake`. Native probes: `--selected=0..3`, `--drag=-90..90`,
plus `--time-ms=N --capture=NEW.ppm`. Use `compare_layers.py` with the matching
case and capture prefix. Review `/desktop-preview/comparison/?layer=full`.

## H106 240×240 screen adaptation

H106 Tiga and Zero desktop layouts and Tiga ST7789 board definitions in the
local `firmwares` repository confirm a 240×240 physical display. This GizOS
checkout does not contain the H106 production project/board assembly; the new
target is a native desktop screen adaptation, not an H106 firmware package.

```sh
bazel run //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel-h106
```

The original AMOLED target and reference stay unchanged. H106 runs the same Lua
animation and effect state machines on a physical 240×240 RGB565 framebuffer:

| Group | Uniform scale | Translation x,y from original design |
| --- | --- | --- |
| Arena, walls, particles, dust | 240/368 | 0,-30 |
| Opponent | .52 | 24.32,6.2 |
| Hands | .62 | 5.92,4 |
| Player HUD | .57 | 1.3,-24.49 |
| Enemy HUD | .57 | 27.94,-.55 |
| Carousel, charge cells and skill icons | .60 | 9.6,-31 |

Each group preserves image proportions, ray widths, blur, orbit angles and
animation timing. The HUD panels have 7px/8px side padding and share top y=8;
each is108.3px wide (about10% larger than the first H106 layout), with8.4px
between panels. Hitboxes and effects use the same enlarged transforms;
the opponent's feet and arena center are around y=144. The bottom icon center is
(120,209), radius166.8 with off-screen center y375.8; cells share the carousel
transform. Touch zones and drag distances use inverse transforms rather than
stale AMOLED coordinates. No change to combat rules, pairing or hardware input.

Verified: both native targets build; Lua/runtime and app suites pass, including
all H106 isolated/combined layers, physical framebuffer bounds, six meter
probe-vs-click comparisons and both carousel directions. The AMOLED full frame
at1875ms remains byte-identical to the prior approved native capture. Actual
H106-sized native window self-test: PASS29.5FPS, steady approximately30FPS;
real charge clicks observed. This does not certify performance on H106 silicon.
H106 screenshot capture uses a 240×240 PPM header and actual PAL pixel buffer.
