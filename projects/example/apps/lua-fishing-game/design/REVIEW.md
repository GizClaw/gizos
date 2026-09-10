# Native visual review

The accepted storyboard PNGs here are design references only. They are not
included by either Bazel target and cannot be loaded by the fishing Lua code.
Actual captures are in `../validation/cq-circular/`.

| Requirement | Native implementation / check |
| --- | --- |
| AMOLED desktop | Native SDL/PAL window, framebuffer exactly 368 × 448 |
| Sea is the home screen | Default `idle`; no text, hands, buttons or HUD over the sea |
| Sea/sky, simple pixel style | Flat blue bands, rectangular clouds, short pixel wave lines |
| Rod butt at bottom-right | All poses start at pixel (367, 447); capture assertions verify corner occupancy |
| Thin rod, no hands | Thin outlined ochre shaft, minimal grip and small reel |
| Boat points upper-left | Small polygon bow entering from bottom-right, diagonal deck seams |
| Baitcaster above / spinning below | Reel geometry uses a rod-local coordinate frame |
| Fly reel at lower butt | Narrow spool behind forward cork grip, close to screen corner |
| Overhead / pendulum / ISO / fly / fighting | Six native storyboard captures; fly loops stay inside screen |
| Power vs Action | Separate values; compliance vs curvature-onset controls, 1,944 bounded poses tested |
| Wooden inventory | Procedural plank rectangles, translucent-looking tint over planks, three tabs |
| Thumbnails / length labels | 3 × 3 slots; lengths in upper-right; no large preview pane |
| English details / brand placeholders | SHIMANO / DAIWA / ABU geometric pixel marks; Length, Power, Action, Model, Capacity |
| Compatibility | Nonmatching reels/lures are gray and unequippable; incompatible equipped slots clear on rod change |
| Navigation | Left swipe enters inventory; right swipe returns to sea; tapping boat does not open menu |
| No textures | Runtime Lua references no PNG/JPG/bitmap/atlas; all display calls are primitive geometry |

Visual review uses the native framebuffer comparisons in `validation/cq-circular`.
The sea band heights, clouds and rod poses follow the approved storyboard. The
bow now has a broad ivory gunwale, inset shadow and perspective deck planks.
Inventory reels have separate oblique spool/frame geometry, crank hardware,
accent bands and perforated fly spools. Lures have layered back/belly colors,
eyes, diving lips, paddle tails and hanging curved treble hooks. Small mounted
reels remain independently authored so inventory detail does not enlarge them.

The reference landscape scenes are adapted to the 368 × 448 portrait target;
these are not pixel-identical reproductions. All reference images remain offline
review materials. No textures or generated bitmap assets enter the runtime.

Validation: native build passes; fourteen actual framebuffer captures pass size,
corner, sky, compatibility, and gesture-handler assertions. `--check` runs tests
inside the same Lua job, not a separate visual mock. Desktop mouse gestures use
that handler; six-axis input and complete fishing gameplay remain future work.

Catalog integration: all 28 researched entries are selectable, plus Hydros IV
(8WT compatibility) and the existing CUSTOM A-WA rig. Four reel silhouettes now
include a separate golden round baitcaster. Paging, every cell, immutable specs,
casting/spinning mounts and fly WT compatibility run in native Lua assertions.
