# H106 approved typography implementation

## Scope

Implements the user-approved 240 × 240 storyboard in the shared Lua application. Equipment keeps four cards and exactly two information rows: brand beside model, followed by a compact description. All 48 full catalog names fit; GAMAKATSU MASTER MODEL II KUCHIBUTO and MEGABASS DESTROYER P5 THE X-BITES use compact 9px model text, with other models at 11px. Brands are 9px and descriptions 11px. Time is 13px, temperature 11px, and the wind code remains the existing AMOLED wave glyph. Settlement retains its wood, fish, shadows and separator, without new background panels or a Record/back label; its name is 13px and primary values 14px, with width fitting for longer values.

No physics or input changes were made for this typography task. The earlier native-resolution and performance changes in this dirty worktree remain intact. No device was flashed in this task; desktop results are not hardware frame-rate or audio-output evidence.

## Reproduction

The tested Lua source SHA-256 is `26e1470f804268231b05e8ea77cb23bf95d09f4a3e48d565371ab50a340e2530`. Both desktop binaries build successfully. For bounded assertions, pass both a fixed time and a fresh capture path; `--check` alone runs assertions and then continues the game.

```sh
SDL_VIDEODRIVER=dummy bazel-bin/projects/example/targets/cc_binary/lua-fishing-game/example-lua-fishing-game-h106 --check --profile=amoled --time-ms=1000 --capture=/tmp/h106-type-check-new.ppm
SDL_VIDEODRIVER=dummy bazel-bin/projects/example/targets/cc_binary/lua-fishing-game/example-lua-fishing-game --check --profile=amoled --time-ms=1000 --capture=/tmp/amoled-type-check-new.ppm
python3 projects/example/apps/lua-fishing-game/tools/verify_h106.py --binary bazel-bin/projects/example/targets/cc_binary/lua-fishing-game/example-lua-fishing-game-h106 --out /tmp/h106-type-layout-new
python3 projects/example/apps/lua-fishing-game/tools/verify_cache.py --layout h106 --info-only --binary bazel-bin/projects/example/targets/cc_binary/lua-fishing-game/example-lua-fishing-game-h106 --out /tmp/h106-type-info-cache-new
python3 projects/example/apps/lua-fishing-game/tools/verify_h106_buttons.py --mode desktop --binary bazel-bin/projects/example/targets/cc_binary/lua-fishing-game/example-lua-fishing-game-h106 --out /tmp/h106-type-buttons-new --target-fps 25
python3 projects/example/apps/lua-fishing-game/tools/verify_desktop_input.py --binary bazel-bin/projects/example/targets/cc_binary/lua-fishing-game/example-lua-fishing-game-h106 --out /tmp/h106-type-touch-new
```

## Checks

H106 and AMOLED assertion runs pass, including 48 equipment labels, 30 fish names, 165 extreme rod poses, 297 fight-rod poses and cast-trajectory equivalence. Nine actual H106 captures include both longest names, all three equipment pages, the sea, the four-card bag and the settlement. The 18 information-panel cache comparisons pass with zero differing pixels. The SDL input test passes all 13 checkpoints and two real simulated casts/splashes. Runtime-button tests use the 4 MiB firmware Lua budget and cover all eight horizontal transitions, selection/scroll, charge, hook, fight, lift, settlement exit and the caught-fish bag.

The existing full-page cache comparison does not pass: RODS at scroll 0 has 274 differing pixels, located at x=47–183, y=50–156 in the equipment thumbnails, outside the changed information panel. This is not reported as a full-screen equivalence pass. `--info-only` is an explicit scope option; the default full-page test and its zero-pixel H106 tolerance remain unchanged. Logs and the nine-screen preview are retained locally under `build/h106-ui-approved/`.

## Audio source confirmation

The six event sounds (bite, hook, drag, caught, lost and slap) are synthesized by `init_fishing_audio()` in Lua using a sine wave and decay envelope. Initialization generates and caches signed 16-bit little-endian mono PCM, requesting 16 kHz and using the output's reported sample rate. Playback writes the cached PCM to the audio interface. No external WAV, MP3 or other recorded-audio resource is used by the fishing game; fixed-frame/check runs intentionally skip audio initialization.

## Follow-up: catch text proportions

Source SHA-256 `1bf852028cb92584714d53d3b38a418c024cd50b5e22c48bb9cfbead0a661138` reduces the fish-bag name from 14px to 12px and raises its numeric details from 7px to 11px. Settlement unit labels increase from 9px to 11px; its 13px name and 14px values stay unchanged. Bag weights consistently show two decimals. Four cards, dividers, the coin icon, wood and fish rendering are unchanged; no extra panels or return prompt are added. AMOLED and physics are unchanged.

Both desktop binaries build and complete their bounded `--check --profile=amoled` runs. `FISHING_H106_CATCH_TEXT_CHECK` verifies 30 bag names and numeric columns, including maximum reference weights and values. Nine actual H106 captures pass; the bag and settlement captures were visually reviewed and retained with check logs under `build/h106-catch-type/`. This follow-up did not flash hardware or rerun the full Runtime-button automation; the earlier full-flow results above belong to the previous source hash.

## Subsequent hardware handoff

The catch-text source above was subsequently installed as v28-test on H106. The user confirmed normal game behavior and accepted committing the changes. Two automated runs remain incomplete because diagnostic output stopped near fighting while the user still observed animated fish and sea. See [the current firmware handoff](H106_FIRMWARE.md#current-handoff-approved-catch-typography-2026-09-13) for installed hashes, partial measurements, build gates and the distinction between manual acceptance and automated coverage. No full-device 25 FPS pass is claimed.
