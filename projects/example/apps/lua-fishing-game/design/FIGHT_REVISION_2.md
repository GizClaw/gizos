# Fight revision 2 — underwater arrivals and deck result

Status: implemented in the desktop live pipeline. `fight-revision-1..12` remain review poses; `deck-demo` and real catches share the production deck renderer.

## Implemented behavior

- Sample a 3D arrival position around the lure, including left, right, far and below. Filter depth and origin by species habitat and the seabed. Do not reuse one fixed lateral offset or force every fish to the lure's surface elevation. Derive fish heading from velocity toward the lure.
- Underwater fish are depth-attenuated silhouettes without surface spray or wakes. Only an actual surface crossing creates a surface disturbance. Panels 1–3 are independent encounter alternatives, not three fish in the same encounter.
- Once hooked, present a clearly taut, fine line from rod tip to mouth under load. Reel retrieval removes excess paid line; fish running releases line under drag. Remove decorative curvature and rope-like oscillation. A genuine transient slack event remains a physical condition, rather than the default loaded appearance.
- Reaching the visible rail waterline (screen y >= 400, x >= 260, within 0.65 m of the rail target, mouth shallower than 0.18 m) triggers a separate 0.45 s lift and full-screen deck sequence. Ordinary close retrieval keeps the fish submerged and only a small head cap may show. Remove the separate low-energy/low-speed landing wait and extra landing click. No net and no hands.
- Deck sequence: drop → contact/splash → first body-powered flop → contact → smaller second flop → settle and show catch information. Estimated timing: 0–0.35 s drop, 0.35–0.50 s contact, 0.50–0.95 s first flop, 0.95–1.15 s contact, 1.15–1.50 s second flop, settle by 1.8 s.
- Keep the shadow on the deck while the fish rises. The tail/body push initiates each hop; gravity returns it to the deck. Second hop has lower height and less rotation. Water droplets follow ballistic arcs and leave small wet marks. Do not scale the whole fish as a substitute for body flex.
- Result uses the caught species' Lua geometry and actual length, weight and coin value. Record exactly once; click after the sequence to return to the sea. No imported fish or wood textures.

## Review scenes

1–3: alternate underwater approaches. 4: hooked and tight. 5: sideways pressure. 6: boat-side direct lift. 7–12: full-screen deck sequence and result. Sea panels use the existing BANTAM/CQ geometry; the example result is a spotted seabass, 48 cm / 1.20 kg / 46 coins.

## Reference search

- Pond5, *Snapper Fish Flops on Wooden Dock and Falls into the Ocean*: https://www.pond5.com/stock-footage/item/203667022-snapper-fish-flops-wooden-dock-and-falls-ocean-small-fishing — relevant video identified from search metadata; playback was not verified.
- Pro Sound Effects, *Wet Slaps Fish Flopping Around on Boat Deck*: https://www.prosoundeffects.com/sound-effects/PSE_OX/bLI8R/Wet-Slaps-Fish-Flopping-Around-on-Boat-Deck — description identifies wet impacts, splashing and individual tail hits. No commercial media downloaded or incorporated.

Timing, two-hop choreography and gameplay transitions are original game design choices, not measurements from these references.

## Sunny oblique deck revision

Deck review panels now share an oblique perspective transform with the fish and wet marks. Fish scale increases from 3.35 to 5.25; upper-left sunlight produces warm pale timber, bright flank highlights and a shadow displaced toward the lower right according to jump height. Eye and pattern geometry use the same projection. The live landing state now uses this same renderer and continuous motion curves.
