# Rendering and animation review — 2026-09-11

Scope: portable fishing Lua, desktop host, native capture verifier, and the related design documentation. Reviewed using the generic-code-review repair/verify/re-review workflow. No textures, imported sprites or prerendered backgrounds were introduced.

## Findings repaired

1. The previous landing was a posed boat sprite, with an early distance gate and no deck state. Live catches now enter an explicit rail-side lift, then the shared sunny oblique deck animation. The fish remains submerged during ordinary close retrieval; a small head cap may show. A rigid rotation brings the rod over the visible rail without changing grip position or segment lengths. The rail gate checks world distance, depth and screen position. Clicks cannot skip the lift or either flop; each catch is inserted once.
2. All approach positions used the same lateral offset. Origins now cover random azimuth, radius and submerged depth, constrained by the seabed; heading follows motion. Surface-only lures additionally trigger a rise, surface splash, and dive. Pencil strokes alternate lateral acceleration; popper strokes emit one bounded splash each.
3. The loaded segmented rope was both expensive and visually slack. Hooking takes up slack and reduces it to two endpoints, preserving installed line length, mass, drag, elasticity and physical loss tests. The first reduced implementation accidentally applied the same end-to-end constraint twice and retained lightweight-node axial damping; both were removed. A 6 kg fixture with 2 N drag now pays out several metres instead of being numerically held in place.
4. Deck projection and wood polygons were recomputed on every frame; idle menus and completed results repainted continuously. Deck primitive commands are prepared before input begins, then replayed only in the moving band. The existing framebuffer retains static pixels. Menu redraw is invalidated by tab, focus, scroll, equipment, bag count or slide changes; completed results issue zero draw calls. Wooden panels are grouped by plank instead of issuing a call per row. Fish colors are lit from the actual species palette.
5. Native line drawing could receive coordinates outside the viewport; Lua per-pixel clipping was also costly. A bounded segment clip now precedes each native line call. The screenshot harness now runs its expensive model suite once and includes four deck poses.
6. Audio alignment incorrectly multiplied two frame sizes, and partial writes could replay accepted bytes. Alignment uses the returned PCM frame byte count, and busy writes resume after the accepted prefix. Desktop audio permits an output-only environment without requiring a microphone; it degrades gracefully when no speaker exists. Lua PCM generation is cached outside physics.
7. The expanded regression block exceeded Lua's 200-local limit. Tests now have a separate function scope. Outdated design-only/net statements were replaced with current behavior and explicit remaining limits.

## CPU and memory budget

| Item | Desktop | AMOLED budget profile |
|---|---:|---:|
| Requested render rate | 60 FPS | 40 FPS |
| Fixed physics rate | 240 Hz | 120 Hz |
| Maximum free-line nodes | 150 | 72 |
| Local constraint passes | 12 | 6 |
| Maximum free-line local constraint evaluations/s | 429,120 | 51,120 |
| Hooked-line nodes / passes | 2 / 2 | 2 / 2 |
| Reef contact queries | 20 Hz | 20 Hz |
| Simultaneous surface splash events / particles per event | 8 / 8 | 8 / 8 |

The free-line ceiling excludes the coarse full-span constraint and velocity updates; it is an operation-count bound, not an MCU timing measurement. Physics uses fixed substeps; render rate does not change fish decisions or input amounts. Species decisions happen on action completion, not per render frame. Rod world geometry remains 64 segments. Only visible inventory rows are drawn. Reel meshes are cached by model.

The RGB565 framebuffer is exactly 368 × 448 × 2 = 329,728 bytes (322 KiB), allocated outside the Lua heap. A full frame at 40 FPS requires about 13.19 MB/s of pixel payload before bus overhead; 60 FPS needs about 19.78 MB/s. Deck animation normally repaints a 368 × 240 band (176,640 bytes); first/result frames are full. Static result/menu frames have no dirty pixel upload. There is no second cached background bitmap.

The host allows 256 KiB source and a 4 MiB Lua allocation ceiling; that ceiling is not a reservation or a measured working set. Observed desktop Lua heap samples were approximately 0.7–2.5 MiB across fighting, the warmed deck cache and animation. Audio/native buffers, the framebuffer, runtime and any firmware services must be budgeted separately. The source also includes preview/test functions, so a firmware integration should account for its startup parse cost.

## Verification evidence

- Native build, `git diff --check`, and 18 native-frame visual captures pass.
- Both desktop and AMOLED profiles pass the deterministic cast/retrieve, equipment/gesture, species policy, actual catch, drag payout, no-small-fish-run, slack/overload loss, surface-lure and 30/120 Hz consistency checks. The rail regression additionally checks that a fish farther out is not landed, while a fish at the visible rail gets an explicit lift before insertion into BAG.
- Actual desktop SDL: casting/waiting approximately 58–61 FPS, fighting approximately 61–62 FPS with 17 ms p95 frame intervals. An isolated deck run measured 58.2 FPS over the animation-containing interval with 19 ms p95; settled frames draw nothing. These measurements are from the developer's Mac, not the ESP32-S3.
- The AMOLED profile on SDL requests 40 FPS and observed roughly 39–40 FPS. This verifies scheduling and functional behavior only.
- 111 actual SDL frames of the shared deck renderer were exported at 60 simulation frames/s. The retained final frame equals a clean standalone render pixel-for-pixel. Export wall-clock FPS includes disk writes and must not be reported as interactive FPS.
- Synthesized audio output successfully opens in the isolated desktop run. Audible quality and device latency were not subjectively verified.

## Final review and limits

Fresh review after the above fixes found no further blocking defect in this desktop scope. The implementation now provides an explicit bounded AMOLED profile, but there is no fishing-specific AMOLED firmware target or on-device frame-time/PSRAM measurement in this change. The board configuration is ESP32-S3 at 240 MHz with PSRAM and a 368 × 448 SH8601 QSPI display; a Mac benchmark cannot establish that this firmware will sustain 30+ FPS. Hardware acceptance must measure render/physics time, display transfer time, peak heap and long-run input responsiveness with the actual firmware services enabled. Do not describe this revision as hardware-certified.

Fish behavior, buoyancy, line strength presets and near-boat guidance are calibrated game approximations. There is one simplified coastal habitat, and catches are session-local rather than persistent. No firmware was flashed and no commit or push was performed.

## Weather extension

The sea scene now samples a fictional day/weather palette once per second while clouds, waves and rain move every rendered frame. HUD layout: 12-hour AM/PM time at top left; zero to three stepped wind strokes and Celsius temperature at top right. There is no weather text label or wind meter. Wind stroke count and sea motion reflect the internal wind strength.

Bounded additional work: eight sky rectangles, at most seven block clouds, thirteen night stars, eleven reflection strips and thirty-six rain strokes with up to fourteen brief surface ripples. All are Lua primitives and reuse the existing framebuffer. The time model wraps after 1,800 real seconds; cloud coverage, precipitation and temperature cooling blend together, independently of wind. The sun is partially visible at the right edge in the afternoon and clipped below y=203 at sunset. Checks cover wraparound, offscreen morning sun, partial afternoon visibility, horizon crossing and rain differences. Six native weather captures were inspected. Hardware frame rate still requires an AMOLED board measurement.

Wind uses seeded daily targets, smoothly joined across midnight. The prevailing wind is randomized per session; daily targets stay within ±0.35 internal wind units of it, limiting any day's variation to 0.7 units (at most one adjacent icon level). Wind is independent of sunny/cloudy/rain transitions and continues to affect waves and feeding. Temperature follows the time-of-day curve plus smoothly blended cloud/rain cooling (up to 3 C).

Rain uses unequal falling speeds, irregular lanes and depth-dependent length/contrast, plus brief perspective-scaled water impact rings. Precipitation fades over weather transitions; it does not force the daily wind to change.
