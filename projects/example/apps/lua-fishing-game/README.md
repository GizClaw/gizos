# Fishing Game — AMOLED desktop preview

Native SDL/PAL + the existing Lua runtime, at **368 × 448**. All scene, equipment,
wood, thumbnails, logos and pixel font are drawn in Lua using rectangles, lines
and scanline polygons. There are **no runtime textures, image assets or atlases**.
The PNGs under `design/` are approved visual references only, not build inputs.

```sh
bazel run //projects/example/targets/cc_binary/lua-fishing-game:example-lua-fishing-game
```

- Starts on the sea. Drag **left** to open gear, **right** to return to the sea.
- Click RODS / REELS / LURES, then a thumbnail to equip it.
- Incompatible items stay visible and gray; clicking one shows its details but
  does not equip it. Changing rod type clears incompatible reel/lure selections.
- Use the side arrows or swipe vertically to page through the inventory.
- Click the rod detail strip to alternate basic specs and line/lure ratings.
- Manufacturer Brand / Series / Model / Power / Action are fixed per model.
  Casting rods accept low-profile or round baitcasters; spinning rods accept
  spinning reels. Fly reels must fit the rod's WT rating.
- Catalog: 9 rods, 11 reels, 10 lure/rig entries. This includes every researched
  item, plus Hydros IV for the 8WT fly rod and the existing CUSTOM float rig.
  Unknown specs display N/A; simulation tuning is not a manufacturer claim.
- Click the sea to rehearse a cast. This is a visual prototype: six-axis gesture
  recognition, fish AI, fighting/reeling mechanics and persistence are not yet
  implemented. The rehearsal goes through cast / wait / tension poses.
- Escape closes the desktop window.

`--scene=demo` cycles six storyboard poses. Individual previews:
`idle`, `overhead`, `pendulum`, `iso`, `fly-back`, `fly-send`, `fight`, `rods`,
`reels`, `lures`, `cq`. `--rod=1..9`, `--reel=1..11` select catalog items.
`cq` renders a standalone CQ pixel-art board; reel indices 10/11 select CQ
100 RIGHT / 200HG RIGHT. The former stat/brand override flags were removed.
The `iso` / `fly-*` scene presets select the appropriate demonstration outfit.

```sh
bazel run //projects/example/targets/cc_binary/lua-fishing-game:example-lua-fishing-game -- \
  --scene=overhead --time-ms=1000 --capture=/tmp/fishing-overhead.ppm --check
```

Captures tap the **actual RGB565 pixels submitted to SDL**, not a second renderer.
Capture paths must not already exist. `--check` executes compatibility, bending,
1,944 rod-pose checks and gesture/equipment-transition assertions inside the same
Lua VM before rendering. For fourteen native captures and contact sheets:

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
