# Vector artwork review — samples visually approved

The user authorized character and skill-icon vector redraws on 2026-09-10,
requesting frame-by-frame comparison and the closest possible match to the old
textures. This review concerns the initial artwork samples. It does not change
the original desktop acceptance baseline or claim firmware migration complete.

## Implemented samples

- Four independently authored skill icons: charge, wave, absorb and guard.
- One opponent in the original standing pose, with named head, torso and limb
  groups, armor planes, helmet spikes, knuckles and purple emission seams.
- SVG paths, gradients, ellipses and clips only; no embedded raster, XML script,
  external resource reference, per-pixel tracing or filter element.
- The five current SVGs total 210,192 bytes; individually zlib-compressed text
  totals 17,872 bytes. These are artwork measurements, not a compiled native
  resource format, final firmware size or measured runtime memory requirement.

Run the two authoring generators from the repository root:

```sh
python3 projects/example/apps/lua-qi-duel/app/tools/generate_vector_art.py
python3 projects/example/apps/lua-qi-duel/app/tools/generate_vector_character.py
python3 projects/example/apps/lua-qi-duel/app/tools/vector_review_server.py
```

Open `http://127.0.0.1:4189/desktop-preview/vector-review.html`. The server binds
only to loopback. Its one report-writing endpoint accepts a bounded same-origin
request, writes only `validation/vector/artwork-review.json`, and records input
SHA-256 identities. “检查并保存 900 张样式帧” updates that report. Asset generation
does not alter or delete the original raster references.

## What has actually been compared

The page uses the original source sheet and existing H2RS atlases. It retains
the original 34 focus samples, including the transition immediately above 0.5,
three directional fades, palette tinting, and 21 confirmation/locked samples.

- 408 focus styles, 408 tinted styles and 84 confirmation/locked styles: 900
  distinct resource-style comparisons, with every difference retained.
- Control check: replay all 408 old focus styles from the old sheet using the
  original browser formulas. All 408 have zero premultiplied RGB difference.
- Opponent comparison at 81, 156 and 321 pixel draw heights.
- Each height uses an independent SVG render result. Reusing one browser SVG
  image across sizes changed the small-size result after enlargement; the
  revised review keeps the 156-pixel result unchanged before/after the report.
- Side-by-side, 50/50 overlay and amplified difference displays; native CSS
  pixel dimensions and an enlarged inspection view.
- XML allowlist/reference checks and JavaScript/Python syntax checks passed.
  `validation/vector/vector-asset-audit.json` identifies the audited assets.

The saved report includes premultiplied RGB mean absolute error over the union
of pixels with alpha above 16, alpha-coverage intersection-over-union and the
number of differing visible RGB pixels. These metrics are diagnostic only.
High alpha overlap is not evidence that highlights, contours or materials match.
Style-average error also includes dim/unselected states and is not a visual
acceptance score. There is no automatically relaxed pass threshold.

## Visual findings and decision gate

The silhouette and fighting pose are recognizable. The draft still simplifies
the original metal/glass facets and soft emission. The absorb vortex differs
in the distribution of its arms and arrows; charge's ring and central crystal,
guard's bevels and the opponent's armor highlights also remain visibly different.
On 2026-09-10 the user reviewed these concrete samples and approved them:
“OK，矢量样稿我看了，没有问题”. The current material style may guide the remaining
redraws. `validation/vector/visual-approval.json` records this decision against
the exact SVG hashes. Earlier metric reports retain their original draft status
as historical evidence. The later native desktop acceptance is recorded separately below.

## Current implementation

This document records the initial sample review. The accepted hands, component
paths, native compiler and full desktop renderer have since been integrated.
[VECTOR-DESKTOP.md](VECTOR-DESKTOP.md) and `validation/vector/component-acceptance.json`
contain the current desktop acceptance state. The 900 resource-style comparisons
here remain historical evidence rather than 900 game-time frames.
ESP32 rendering, physical timing and long-duration memory validation remain pending.
