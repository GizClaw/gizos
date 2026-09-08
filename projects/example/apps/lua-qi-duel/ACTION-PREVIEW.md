# Action rehearsal v1

The default live app now runs the mode menu and game described in `GAMEPLAY.md`.
This document describes the retained `--rehearsal` / inspector animation mode.

This is the actual Lua/SDL desktop app, not the browser reference. Both desktop
targets depend on the same portable app, Lua timeline, C compositor and packed
art resources. The existing AMOLED firmware component also depends on that app.
No device build, flash, memory optimization or FPS guarantee is part of this pass.
H106 hardware integration remains separate unfinished work.

## Controls

Both native desktop targets enable `click_controls`: click the bottom wheel's
left region to rotate left one position, right region to rotate right one
position, or center to cast the current skill. These are physical rotation
directions (right is clockwise). A click commits on release in the same region;
motion beyond 12 design pixels cancels it. The echo expands in place and fades
to zero over 620 ms; busy clicks do not restart the cast or echo. Meter clicks
remain separate. Desktop no longer requires dragging a skill to cast.

The default firmware touch policy is unchanged:

- Drag left/right across the bottom wheel, then release, to select a skill.
- Start on any visible skill icon; drag predominantly upward at least 44 design
  pixels (26.4 physical pixels on H106), then release, to cast that icon's skill.
- Short upward gestures and downward gestures cancel. Starting away from an
  icon cannot cast. A horizontal gesture locks to wheel rotation, preventing a
  diagonal wheel swipe from accidentally firing a skill.
- The icon stays in its orbit. An enlarged translucent echo grows in place as
  the pointer moves upward; the chevron turns white at the threshold.
- Releasing continues from the exact drag echo scale/opacity, expands it
  further and fades it in place over 620 ms with cubic opacity falloff, and starts the skill
  timeline. The icon never flies upward. Repeated casts
  during an active animation are ignored, rather than queued for later firing.
- This is a visual rehearsal: both combatants demonstrate the same selected
  skill, with no energy cost, damage, Bluetooth or AI. Existing meter test taps
  remain available. CLI `--actor=player|opponent` isolates one actor in a probe.

The meter now has five octagonal cells. The base and cell orbit share a 78%
uniform scale about the lower anchor, keeping the transparent material and
pulling the edges inward away from the hands. Cells retain the original blue
crystalline sprites and are 1.65x wider along the orbit tangent, with unchanged
height; empty, filled and transient sprites share the same transform. The base
remains static even at 5/5; no full-charge flame or level-dependent tint. Click the
left/right halves of the charge arc to decrease/increase. `--qi=0..5` selects an
initial value for native inspection. Both screen profiles share this Lua path.
The echo reaches zero opacity at its maximum size and is then removed; the
stationary selected icon remains visible underneath.

H106 also consumes Runtime BUTTON_DOWN/UP for logical Volume+ (9), Volume- (10),
and Record (11). Volume+ selects next, Volume- selects previous; Record casts
with the same in-place expanding echo. One action per physical press; duplicate
DOWN and the separate BUTTON_ACTION event do not repeat it. Touch remains enabled.
Desktop maps these to `Up`, `Down`, `Tab` and peripheral IDs 106/107/108, matching
the H106 Tiga desktop convention. The private product launcher must supply the
equivalent logical mapping on hardware; this change does not flash a device.

## Animation

Each generated sheet contains four rows (charge, wave, absorb, guard) and four
pose columns. Original idle art is retained outside the action. Intermediate
poses switch at authored times; continuous bob/recoil, inward particles and
energy streams update every display frame. This is a first four-pose rehearsal,
not a completed high-frame-count animation or skeletal interpolation. Poses are
not crossfaded: that would double fingers and limbs.

| Skill | Windup | Hold | Return |
| --- | ---: | ---: | ---: |
| Charge | 360 ms | 820 ms | 300 ms |
| Wave | 480 ms | 620 ms | 320 ms |
| Absorb | 360 ms | 1000 ms | 360 ms |
| Guard | 230 ms | 800 ms | 280 ms |

Absorb returns directly from the Tai Chi hold to default: there is no
lower-to-abdomen recovery. Guard adds intermittent short contact flashes.
Wave uses independent tapered beam strips, white core, colored outer light,
traveling bright streaks, palm sparks and collision glow. Palm anchors are
authored in the same design space as the pose sprites. In two-actor rehearsal,
the beams converge; in a single-actor probe they travel toward the other side.

## Reproduce

```sh
bazel run //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel-h106
bazel run //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel

# Deterministic real PAL frame; choose a new destination for each capture.
bazel run //projects/example/targets/cc_binary/lua-qi-duel:example-lua-qi-duel-h106 -- \
  --action=wave --actor=player --time-ms=650 --capture=/tmp/qi-duel-wave-check.ppm

bazel test //libs/lua:lua_test //projects/example/apps/lua-qi-duel/app:lua_qi_duel_test
```

Tests exercise both resolutions, all four actions, real PAL DOWN/MOVE/UP and
DOWN/UP flicks, cancellations, changing effect frames and exact return to the
original idle composition at 1800 ms. Existing HUD, charge and carousel tests
remain in place.

## Artwork and packing

Built-in imagegen generated the pose sheets from the existing opponent and hand
references. The initial request for transparency returned a painted checkerboard;
those drafts were rejected. A second imagegen edit replaced only that backdrop
with pure black. The retained source sheets are `assets/source/action-*-v1.png`.
The exact prompt specifications are in `assets/source/action-prompts-v1.json`.

`app/tools/pack_action_atlas.mjs` imports the black-key sheets, flood-clears the
outer black background, normalizes opponent baselines, and packs 16 individually
compressed 192x192 RGBA frames into each H2RS resource. It does not synthesize
limb poses. Fully enclosed black gaps may remain opaque in this first art pass;
proper artist-authored alpha masks can replace the import later if needed.

```sh
# sharp must be available to Node (e.g. through NODE_PATH).
node app/tools/pack_action_atlas.mjs assets/source/action-opponent-v1.png assets/generated/action-opponent.h2rs
node app/tools/pack_action_atlas.mjs assets/source/action-hands-v1.png assets/generated/action-hands.h2rs
```

Visual correctness is the acceptance criterion now. Device memory/bandwidth,
texture cache behavior and reduced-resolution exports are deliberately deferred
until the motions are approved. Identical renderer input does not imply identical
panel color response; compare geometry/timing first and calibrate hardware later.
