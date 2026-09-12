# Fishing Game — AMOLED / H106

Native SDL/PAL + the existing Lua runtime, at **368 × 448**. All scene, equipment,
wood, thumbnails, logos and pixel font are drawn in Lua using rectangles, lines
and scanline polygons. There are **no runtime textures, image assets or atlases**.
The PNGs under `design/` are approved visual references only, not build inputs.

```sh
bazel run //projects/example/targets/cc_binary/lua-fishing-game:example-lua-fishing-game
```

## H106 layout preview

```sh
bazel run //projects/example/targets/cc_binary/lua-fishing-game:example-lua-fishing-game-h106 -- --scene=rods
```

The H106 desktop target presents **240 × 240** pixels. Equipment uses a **2 × 2**
grid, enlarged shared thumbnails, and matching scrolling/hit testing. The sea,
three equipment labels, bottom details, and standalone BAGS module keep their
AMOLED composition. H106 BAGS also uses a 2 × 2 grid (AMOLED stays 3 × 3).
H106 rasterizes vectors directly into **240 × 240**, with no intermediate 448 × 448 image or framebuffer filtering. Shared authored coordinates still use 448 units; the desktop adapter only maps pointer input back to those coordinates. The boat and rod retain their shape and right-bottom anchor.

The information panel keeps its existing height and uses exactly two rows: a 9px brand wordmark beside the full 11px model name, then an 11px compact description. The two longest model names use 9px compact glyphs to fit the 212px content width. All 48 catalog items are checked for row width and glyph coverage. Glyph cells are integer physical-pixel rectangles cached as drawing commands, not imported textures or filtered text.

H106's time uses 13px text and temperature uses 11px text. Wind retains the shared AMOLED wave glyph and level mapping. Settlement keeps the original wood, fish, shadows and separator, with a 13px fish name and 14px key values; long numeric values fit their columns. It adds neither information backgrounds nor a Record/back label. These typography changes do not change AMOLED rendering, the four-card bag, input behavior or physics.

Fish-bag typography uses a 12px name and 11px length, weight and value, replacing the previous 14px/7px contrast. Settlement units are also 11px, closer to its 14px numbers. All 30 species are checked against their bag-name and numeric-column widths, including maximum reference weights and values; the four-card grid, dividers, coin icon and existing backgrounds remain unchanged.
The normal desktop preview retains mouse drag/click interactions. H106 firmware uses the button controls below. Desktop timings are not hardware performance evidence.
`--profile=amoled` still selects the existing physics budget, not a display layout.

The target supports the same `--check`, `--capture`, `--scroll`, and scene options
as AMOLED. Native viewport identity and input mapping are covered by
`bazel test //projects/example/targets/cc_binary/lua-fishing-game:h106_preview_test`.

H106 live fishing uses an inset rod/reel anchor (the boat remains corner-anchored),
a larger reel, and continuous coverage-based rod strokes. This avoids the old
hard-quad/center-line seams; it does not change world-space fishing physics.
Missing equipment is explicitly labeled on the sea screen.

The H106 desktop target defaults to an **8 MiB Lua limit**; firmware and the bounded button-test runner use **4 MiB**. H106 retains native-resolution inventory page caches across navigation and draws the current scroll rail instead of precompiling every possible scroll position. Entering inventory releases unused sea and settlement surfaces, including the native background reference. `--vm-kib=4096` reproduces the firmware Lua budget on desktop, but is not a substitute for device heap validation. AMOLED keeps 4 MiB.

For a bounded end-to-end desktop input test:

```sh
python3 projects/example/apps/lua-fishing-game/tools/verify_desktop_input.py \
  --binary bazel-bin/projects/example/targets/cc_binary/lua-fishing-game/example-lua-fishing-game-h106 \
  --out /tmp/h106-input-new-run
```

This injects SDL mouse events at **physical 240 × 240 coordinates**, passes them
through the normal SDL queue, H106 inverse touch mapping and Lua polling loop,
and requires 13 checkpoints plus two actual casts/splashes. It captures real
submitted frames and writes a report. It reproduced a 4 MiB out-of-memory failure
when opening REELS after changing rod and returning to the sea. Unlike `--check`
or `device-bench`, it does not directly invoke game gesture handlers. It does not
test macOS mouse delivery or firmware input. Use a new output directory each run.

## Shared interactions

### H106 button controls

Volume Up cycles **Sea → RODS → REELS → LURES → BAGS → Sea**; Volume Down reverses the cycle. Left/Right selects the previous/next item and keeps its complete card visible. Selection follows the existing compatibility rules; choosing a different rod can leave a reel or lure unequipped.

Button selection now animates the focus frame and automatic row scrolling over
240 ms, retargeting from the current position for repeated keys. Rod thumbnail
caches no longer depend on the selected rod; reel/lure variants use their actual
compatibility mask. H106 prepares a visible neighborhood instead of eagerly
rasterizing the complete catalog. Tab timing begins after its first presentation,
so cold preparation does not consume the animation's duration.

H106 text retains its final-screen pixel alignment. Lua defines monochrome glyph
cells; `display.draw_commands(handle, top, bottom, x, y, sx, sy, color)` replays
them in the native rasterizer. Optional transform/recolor arguments preserve the
old untransformed fast path, and glyph positions match the prior per-cell Lua
implementation. No bitmap artwork or font file was introduced.

The button verifier records `H106_UI_ANIMATION` first-response latency, submitted
intermediate frames, scrolling frames and frame gaps. It visits five items and
returns to the first item in each equipment category. `H106_PHASE_PERF` counts
changed submissions during each active phase; static menu and post-reveal
settlement loops are excluded. Reports distinguish functional pass from the
30 FPS threshold; desktop numbers are not hardware acceptance.

`H106_TAB_ANIMATION` separately records all six directional RODS/REELS/LURES/BAGS
routes and both LURES↔BAGS directions after a real catch. First-frame latency is
reported separately from the subsequent animation FPS. Nonterminal check failures
are retained as failures while allowing later stages to be measured.
The complete v22 hardware run passed all 15 functional checkpoints but **failed
30 FPS acceptance**: horizontal animations 7.9–8.6 FPS (first frame 101–925 ms),
fight 6.9 FPS, settlement 15.3 FPS. See [the hardware report](validation/H106_FIRMWARE.md)
for all phases, memory fixes, and the unresolved earlier charged-cast inconsistency.

On the sea, hold Record to charge for up to 1.4 seconds, then release to cast. Charge is measured from Runtime event timestamps, not render timing. Shorter holds cast shorter distances. Record taps retrieve the lure, hook a bite, reel during a fight, complete landing, and leave settlement after its reveal animation. During a fight, holding Left/Right changes the rod angle without adding a reel click. Changing pages cancels an unfinished charge.

For lure/ISO casts, charge scales the free line's release velocity coherently,
while retaining the normal rod-stroke timing. Fly casts retain their false casts
and model a final hand haul across the whole fly line, not the fly alone. Neither
mode accelerates the animation: doing so changed the rod's release phase and
could make stronger casts shorter. Five charge levels are checked for all 11 rods.
H106 rasterizes at native resolution and submits changed regions; continuous rod coverage uses single-precision screen-space math without changing world physics.

All geometry and bitmap-font definitions remain in Lua. The native rasterizer draws these definitions directly into 240 × 240 pixels without a framebuffer area filter; cached procedural frames are not imported textures. Firmware supplies display storage from PSRAM with internal DMA staging buffers.

`--button-check --profile=amoled` on the H106 desktop binary runs the same opt-in diagnostic as the H106 test firmware. It injects typed button events through Runtime's exclusive test-input session and waits for actual Lua callbacks. It covers page navigation, automatic scrolling, two charged casts, tap retrieval, rod steering, landing, settlement exit and the recorded catch in BAGS. The fish encounter is an explicit deterministic fixture; hook/fight/landing are real simulated gameplay, not direct state completion. The test ends with `H106_AUTO DONE PASS physical_input_restored` and closes the test-input session. Production firmware does not register the test module. This does not verify physical ADC thresholds or switch debounce.

### Touch controls (AMOLED and normal desktop previews)

- Starts on the sea. Consecutive **left** swipes follow
  **Sea → RODS → REELS → LURES → BAGS**; right swipes reverse that route.
  BAGS is a standalone fish-bag module alongside equipment, with its own header,
  not a fourth equipment tab. The route does not wrap at either end.
  Inventory content follows the finger and settles with a 180 ms slide.
- The equipment header contains only RODS / REELS / LURES. Swipe horizontally
  to change pages (including across the header), then click a thumbnail to equip
  it. Header taps do not change pages. Browsing BAGS never changes equipment.
- Rod cells show C (casting), S (spinning, including ISO), or F (fly) at the
  upper left. All upper-right lengths use feet/inches rounded to the nearest
  inch, including metric rods; original catalog length remains in the details.
  The equipped indicator is at the lower left of rod cells.
- All 12 brand slots have Lua pixel marks. JACKALL includes the framed jackal
  head. Other marks use brand-specific lettering, colors and strokes; these
  are small pixel interpretations, not bitmap logo assets.
- Incompatible items stay visible and gray; clicking one shows its details but
  does not equip it. Changing rod type clears incompatible reel/lure selections.
- Drag the inventory vertically for continuous scrolling. Each tab remembers
  its scroll position; the tabs and detail panel stay fixed during vertical
  scrolling. Dragging never equips an item; there are no page arrows.
- Equipment details use four fields in two rows: brand logo / series,
  followed by length+power / action for rods, model / line capacity for reels,
  or model / grams for lures. Example rod values: `6'8M` / `RF`.
- Manufacturer Brand / Series / Model / Power / Action are fixed per model.
  Casting rods accept low-profile or round baitcasters; spinning rods accept
  spinning reels. Fly reels must fit the rod's WT rating. Fly rods accept only
  flies; ISO rods accept only the A-WA rig; freshwater/lure rods reject A-WA.
  These are discipline/mount rules, not an artificial freshwater/saltwater
  prohibition on otherwise usable lures.
- Catalog: 11 rods, 19 reels, 18 lure/rig entries. Rods are curated to one brand/model per displayed length; researched
  alternatives remain documented in TACKLE_CATALOG.md.
  Unknown specs display N/A; simulation tuning is not a manufacturer claim.
- Click the sea once to run a complete cast: backswing, forward sweep, elastic
  recovery, line payout, flight, water entry and a lowered waiting pose. Click
  after landing to twitch and retrieve one stroke. Rapidly click four times (gaps at most 650 ms) for sustained A-WA retrieval; isolated A-WA clicks move it only 12 cm. Retrieval stops with 65 cm of leader (85 cm for A-WA); the next click starts a new cast. Vertical casting gestures also remain available; fly casting still requires two reversals. Clicks during casting/flight do not restart it.
  Horizontal left swipe still opens equipment. Vertical flicks optionally set
  the strength of the same automatic stroke; fly flicks require two reversals.
- The pivot is 25 cm behind the grip on its extended centre line. The rod sweeps
  an oblique circular plane in 3D. Perspective preserves near/far size changes.
  Fly rods make repeated false casts with pauses for line turnover; ISO rods
  use a separate pendulum stroke. Fish-load poses remain available through
  `--scene=fight --fish-kg=2`; this casting test does not trigger fish AI.
- After water entry, mass, buoyancy and quadratic water drag govern motion.
  Sinking rigs slowly draw slack out of the fixed paid-out line. Topwater and
  A-WA stay afloat; floating minnows recover toward the surface and suspending
  models retain near-neutral buoyancy. Underwater line and lure silhouettes fade
  with depth. The line is not forcibly interpolated into a straight screen line.
- Render target: 60 FPS, with measured FPS and 95th-percentile frame intervals
  logged every two seconds. Rod elasticity uses 120 Hz integration; the XPBD
  line and lure use 240 Hz, independent of rendering. `--scene=physics-demo`
  repeats a full cast every ten seconds for live performance checks.
  Six-axis IMU input is not implemented yet.
- Changing any equipped rod, reel or lure resets the live simulation and updates
  the scene. No fallback lure or reel is shown when that slot is empty.
- Escape closes the desktop window.

`--scene=demo` cycles six storyboard poses. Individual previews:
`idle`, `overhead`, `pendulum`, `iso`, `fly-back`, `fly-send`, `fight`, `rods`,
`reels`, `lures`, `bags`, `fish-bag-preview`, `cq`. `bags` shows actual catches
(initially empty); `fish-bag-preview` shows the specimen catalog for visual checks.
`fish-bag` remains an alias for `bags`.
`--rod=1..11`, `--reel=1..19`, `--lure=1..18` select catalog items.
`--detail=0..1` is retained for capture CLI compatibility; the four-field
summary is fixed. Inventory
captures scroll to the selected rod, reel or lure; incompatible
items are cleared by the same compatibility rules used in the live menu.
`cq` renders a standalone CQ pixel-art board; reel indices 10/11 select CQ
100 RIGHT / 200HG RIGHT. The former stat/brand override flags were removed.
The `iso` / `fly-*` scene presets select the appropriate demonstration outfit.

```sh
bazel run //projects/example/targets/cc_binary/lua-fishing-game:example-lua-fishing-game -- \
  --scene=overhead --time-ms=1000 --capture=/tmp/fishing-overhead.ppm --check
```

Captures tap the **actual RGB565 pixels submitted to SDL**, not a second renderer.
Capture paths must not already exist. `--check` executes compatibility, bending,
2,376 rod-pose checks and gesture/equipment-transition assertions inside the same
Lua VM before rendering, including the five-page swipe route, endpoint behavior,
and independent read-only Bags browsing. For twenty native captures and contact sheets:

```sh
python3 projects/example/apps/lua-fishing-game/tools/verify_desktop.py \
  --binary bazel-bin/projects/example/targets/cc_binary/lua-fishing-game/example-lua-fishing-game \
  --out /tmp/fishing-review
```

The verifier requires Pillow. Brand marks are geometric pixel placeholders.
Real model specifications and manufacturer sources are recorded in
`design/TACKLE_CATALOG.md`. Unpublished lure weights have separate simulation
estimates; the A-WA rig is explicitly marked CUSTOM / PROTOTYPE.
Power controls compliance; Action controls where curvature begins along the rod.
Rod length, rig weight/drag and the displayed load also influence the shape.
This is a desktop visual check, not an AMOLED firmware or device-performance test.

Latest visual validation: `validation/cq-circular/`. The main scene remains the sea;
CQ art is Lua primitive geometry with separate mounted and inventory shapes.

## Rod physics API

`RodPhysics.new(length_m, power, action)` creates a reduced-order elastic beam.
`RodPhysics.step(state, dt_seconds, input)` integrates handle angular inertia,
lure mass, fish line tension and spring damping. Inputs: `handle_angle` in
radians, `lure_g`, `fish_kg`, `fish_pull` (fraction of fish weight transmitted
as line load), and `drag_n` (reel drag cap). `RodPhysics.points(state)` integrates
the distributed bend into 65 screen points anchored at (367,447). The zero-load
reference curve preserves the approved storyboard control points and tip
(236,123) for a 2.03m rod; physical deflection rotates its tangents.

Power maps to calibrated flexural stiffness and natural frequency; length
affects compliance and rebound speed; action determines the bending region.
These are game-calibrated coefficients, not measured manufacturer blank EI.
Fish load includes buoyancy/pull rather than treating the fish as a dead weight
suspended in air. The current desktop model uses one bending mode; it is not a
full fluid/line/finite-element simulation. Substeps limit frame-time dependence.

`--check` also tests fish-weight/power/length/action responses, rebound decay,
30 vs 120 Hz equivalence, extreme-load stability for every rod, the actual
mouse handler, landing/hook transitions, fly false casts, and gear resets.

### Sea reel projection

All 19 catalog entries have individual geometry profiles, selected by the actual
equipped reel. The sea renderer projects simple 3D parts through one orthographic
side/top camera and sorts visible faces by depth. Spool and crank axes remain
transverse on baitcasting, round and fly reels; spinning spool axes follow the
rod. Casting reels mount above the blank; spinning and fly reels hang below.
The fly reel mounts near the butt and is clipped naturally by the screen edge.
Hydros rings are open geometry. CQ has two circular side plates surrounding a
recessed spool. The accepted large inventory CQ drawing is unchanged.

`--scene=reel-view --reel=1..19 --time-ms=1000 --capture=new.ppm` renders
each complete side/top view independently, including reels incompatible with
the currently selected rod. Preview geometry uses a two-pixel grid; the sea
uses native pixels. Meshes are generated once per model and cached.

Profiles are simplified pixel interpretations, not dimensionally measured CAD
replicas. Reference structures: [Shimano Calcutta Conquest](https://fish.shimano.com/en-SG/product/reels/baitcast/a075f00002k2hkuqay.html),
[Daiwa STEEZ SV TW](https://www.daiwa.com/scandinavia/product/1yctxba),
[Abu Revo SX](https://www.abugarcia.com/products/revo-sx-low-profile-reel-1573498),
and [Orvis Hydros](https://www.orvis.com/product/hydros-reel/3M3R.html).

### Full cast capture

`--scene=cast-demo --time-ms=1800 --capture=/tmp/new-cast.ppm` advances the
actual simulation to a requested time before capturing the SDL pixels.
For a deterministic 60 Hz frame sequence, create an empty output directory and
run `--scene=cast-record --record-prefix=/tmp/new-dir/frame- --record-frames=360`.
This exports six seconds of simulated time; export speed includes disk I/O and
is not a real-time FPS measurement. `--scene=physics-demo` measures real time.

The implementation, sources, calibration limits and verification are documented
in [design/CAST_PHYSICS.md](design/CAST_PHYSICS.md).

Minnow SR/MR/DR and crankbait short/medium/long bills use selected-item Lua geometry in both inventory and live casting. `--scene=lure-view --lure=16 --time-ms=1000 --capture=new.ppm` provides an enlarged procedural preview. Diving-depth ratings apply during retrieval, independently of passive buoyancy.

Fish study: 30 Lua-drawn species in `--scene=fish-atlas`, `fish-atlas-2`, `fish-atlas-3`. Swipe left from LURES to BAG; vertical scrolling browses actual catches. `fish-bag-preview` uses illustrative records. Real catches are stored for the current session only. Weight ranges and fictional coin prices are in `design/FISH_CATALOG.md`.

### Live fishing and deck landing

Click to cast and twitch-retrieve. A committed bite can be hooked with one click. During a fight, click left/right to apply side pressure, or the middle to reel a short stroke. The loaded line is straight; forces still control rod flex, drag payout, slack, hook damage and line failure. Fish arriving near the boat are lifted directly into a full-screen sunny, oblique wooden deck scene, without a net. The actual fish drops, flops twice, settles, and displays its length, weight and coin value. After the 1.8-second sequence, a fresh tap returns to the sea; the following tap casts again. Each catch is recorded exactly once.

After splashdown, the handle lowers over 1.1 seconds toward a rod-length-specific
pose whose unladen tip is 0.30 m above the water. Elastic motion and line tension
still affect the actual tip, and the rope follows that physical point. The launch
and flight are unchanged. Fully retrieving the rig raises it again for the
hanging leader and next cast; hooking retains the existing fighting posture.

Surface-only lures have explicit behavior: SUPER SPOOK JR. alternates lateral acceleration during successive strokes (walk-the-dog); POP-X emits one small splash per stroke. Floating minnows are not treated as surface pencils. Surface feeders rise from below, strike with a surface splash, and pull the rig down; hooking starts a short dive before normal species behavior resumes.

`--scene=fight-demo` drives the same live pipeline automatically for inspection, but can lose fish. `--scene=deck-demo --time-ms=725 --capture=/tmp/new.ppm` captures the actual deck renderer at a chosen animation time, with a clearly designated example record. `fight-revision-1..12` are review poses; these do not replace live validation.

`--profile=amoled` selects a 60 FPS scheduling ceiling, 120 Hz fixed physics, at most 72 line nodes and six local solver passes (two for a hooked fish). This ceiling is not measured performance. Desktop defaults to 60 FPS / 240 Hz / 150 nodes / twelve passes. Both profiles use the same geometry and state machine. Use `--check` with either profile for deterministic regression, including native/Lua cast and underwater comparisons. A desktop run of the AMOLED profile is not an ESP32-S3 performance measurement. The firmware target is `//projects/example/targets/h2loader_tar_zlib/lua-fishing-game/amoled:package`; install it through H2Loader's application upgrade, preserving recovery. See `design/PERFORMANCE_30FPS_WORKLOG.md` for ongoing real-device measurements: the target is not yet achieved for every live scene.

For rendering comparisons, `--no-cache` disables inventory snapshots, sparse weather backgrounds, batched depth paths and the bounded rod raster fast path. It does not disable native physics. `--scroll=N` selects a nonnegative integer inventory offset (clamped to the page's actual range). `tools/verify_cache.py` compares cached/uncached RGB565 captures; only bounded one-pixel thumbnail raster-edge differences are allowed. `perf-demo` is a temporary firmware benchmark with continuous vertical scrolling followed by a cast, not the normal interactive startup scene.

### Fixed inventory chrome and automated hardware measurements

Horizontal inventory transitions keep the wood, information frame and the three
equipment labels fixed. Only foreground content and the selected-tab pill move.
BAGS remains a separate sibling module. Two foreground snapshots, a bounded
24-fish thumbnail neighborhood, compact native transparent runs and retained
deck geometry avoid rebuilding unchanged artwork. The deck's shadow, thickness
and lit fish share an identical pose projection; physics timesteps and node
counts are unchanged.

Equipment information uses compact display aliases at a fixed integer 2x glyph
size, preserving the full manufacturer data in the catalog. For example,
`DESTROYER P5 THE X-BITES` displays as `P5 X-BITES`; reel capacity keeps line
type, strength and length while shortening separators and `BACKING` to `BK`.
All 48 current items must fit both columns without shrinking or truncation.

`device-bench` is a temporary automated test scene. It runs reference/optimized
horizontal gear/BAGS transitions and vertical scrolling, then an actual cast,
hook, fight, lift, unique catch record and settlement. Its deterministic 1.2 kg
bite and 30-specimen browsing fixture are test inputs, not measurements of random
encounter probability or physical touch latency. It drives the game's gesture
handlers, not the capacitive controller. The final repeated settlement phases
reuse the same catch without adding records. Normal firmware starts in `idle`.

```sh
python3 projects/example/apps/lua-fishing-game/tools/run_auto_perf.py \
  --binary bazel-bin/projects/h2loader/targets/cc_binary/cli/h2loader \
  --mode upgrade --port /dev/tty.usbmodem1101 --out /tmp/new-hardware-perf
```

This command installs an **already staged** H2Loader APP package and monitors
the test. Use `--mode monitor` to attach without an upgrade, or `--mode desktop`
with a desktop executable for host-only regression. Do not equate host timings
with ESP32 performance. Reports distinguish phase-loop FPS, changed-frame FPS,
p95 intervals, maximum latency (including first-frame work), CPU drawing and
submission time. Idle loops on a static result page are not animation FPS.

`tools/verify_fixed_tabs.py` checks fixed chrome across 20 native transition
captures. `tools/verify_fish_cache.py` compares 13 settlement poses and six bag
scroll positions against the uncached renderer. Settlement must be pixel exact;
AMOLED pre-rasterized bag hairlines permit only bounded one-pixel clipping-edge
differences. H106 redraws boundary fish to preserve details during downsampling.

### Game weather and time

The fishing HUD shows 12-hour time with AM/PM at top left, and a pixel wave wind icon with Celsius temperature at top right. Wind uses four visual levels: no strokes for calm, then one, two or three wave strokes; sea motion also follows wind strength. Weather is fictional game state, not a live forecast: a full day takes 30 real minutes, starting at 09:00. Auto weather transitions between sunny, cloudy and rainy conditions, blending cloud cover over 25 seconds.

Morning sunlight stays outside the view; the afternoon sun enters from the right and descends behind the horizon around 18:00. Sky, sea, clouds and reflections use time/weather palettes, with stars at night. Artwork remains procedural, with eight sky bands and bounded wave/rain counts. Runtime RGB565 snapshots cache unchanged backgrounds and inventory art; no external texture assets are used.

Preview with `--scene=weather-demo --hour=18 --weather=sunny`; `--hour=0..23` and `--weather=auto|sunny|cloudy|rain` also work in the playable scene.

Wind uses seeded daily targets, smoothly joined across midnight. The prevailing wind is randomized per session; daily targets stay within ±0.35 internal wind units of it, limiting any day's variation to 0.7 units (at most one adjacent icon level). Wind is independent of sunny/cloudy/rain transitions and continues to affect waves and feeding. Temperature follows the time-of-day curve plus smoothly blended cloud/rain cooling (up to 3 C).

Rain uses unequal falling speeds, irregular lanes and depth-dependent length/contrast, plus brief perspective-scaled water impact rings. Precipitation fades over weather transitions; it does not force the daily wind to change.

### AMOLED optimization checkpoint

[Performance work log](design/PERFORMANCE_30FPS_WORKLOG.md) records current
firmware experiments, real-device timings and remaining acceptance work.
[Original handoff](design/OPTIMIZATION_HANDOFF.md) is historical: its 80 MHz
stripe investigation predates the user-confirmed 40 MHz fix. Sustained 30+ FPS
hardware animation has not yet been achieved in every live scene.
