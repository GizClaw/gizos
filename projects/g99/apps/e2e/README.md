# G99 product E2E

This suite drives the production G99 [Main](../main/include/h2_g99.h) end to end. It starts Main through `h2_g99_run()` on its own task, presses the logical HOME, SCREEN, UP, DOWN and OK components through Runtime Test Control, enters text through the public `H2_G99_EVENT_TEXT_INPUT` event, and touches the screen with a scripted finger it hands Main as `h2_g99_config_t.touch`. It asserts only on the published `h2_g99_observation_t` and the `h2_g99_frame_stats_t` Main reports through `h2_g99_config_t.frame`, and never reaches into Main's internals.

The scripted finger opens the target's Runtime touch to validate its capability contract, then owns pointer input for the automated run. Physical touches are tested separately in Main and cannot leak between automated cases. While a case scripts it, it answers Main's touch reads the way a touch controller does: down, a move to wherever the finger is at that moment, or up. A slow Main therefore misses intermediate positions exactly as it would with a real finger. Deliberate drags and taps wait until Main read the touch-down and the end point; flicks move on the clock alone. A lifted finger stays up until Main read it up again 80 ms after the lift, so the next gesture is a new press and not the continuation Main makes of a contact that returns within 50 ms.

Every case starts with a clean finger that reports each change at the next read, like the desktop and Web sources. The controller cases switch it to the Mosaico CST9220 instead, which publishes one report every 25 ms: with driver 0.1.1 a read between two reports finds an empty frame that the board reports as a release, and with driver 0.1.2 that read fails. The finger can also hover, moving without contact the way the Web platform reports a mouse with no button pressed.

## Targets

| Target | Sizes | Run | Capabilities |
| --- | --- | --- | --- |
| [Desktop](../../targets/cc_test/e2e/BUILD.bazel) | 480×480, 320×240 | `cc_test`, headless with `SDL_VIDEODRIVER=dummy` | `wifi` (the layout's simulated station serves `HAIVIVI-MFG`) |
| [Web](../../targets/pkg_tar/web/g99_web.bzl) | 480×480, 320×240 | `browser_test` in pinned headless Chromium | none |
| [esp_mosaico](../../targets/h2_loader_tar_zlib/e2e/espressif/mosaico/BUILD.bazel) | 480×480 | `g99-e2e` app package, build only; running it needs a device | `wifi` (the board station; the device run needs `HAIVIVI-MFG` in range), `frame-budget` |

`frame-budget` means the target holds G99's frame budgets: at rest Main reads touch at least every 50 ms, and active animations keep frame intervals and frame work within 50 ms (20 FPS); a key rotation also draws at least five frames. Shared CI hosts do not declare it, so there the perf cases only report their numbers and the real-time `touch/wheel-flick` is UNSUPPORTED.

Desktop and Web run with every `bazel test //projects/g99/...`; they are hermetic and need no device or network. The Desktop target caps its render allocator at eight RGB565 rows: this exercises the smaller-buffer fallback and fails if persistent LVGL or glyph allocations use that bounded heap. On `esp_mosaico` the suite presses HOME and SCREEN on the AI and BOOT keys and UP, DOWN and OK on virtual push-edge Buttons that only Runtime Test Control drives; the image reads its selection from `/data/e2e/select.txt` and prints `H2_G99_E2E_DONE exit_code=<n>` after the summary. The suite stops Main before returning; the retained LCD frame after `DONE` is not interactive. Use the Main image for physical dragging and the AI return key; automated key injection does not qualify the GPIO path.

```sh
bazel test --config=macos_arm64 //projects/g99/targets/cc_test/e2e:all
bazel test --config=macos_arm64 //projects/g99/targets/pkg_tar/web/e2e/...
```

To watch a desktop run, start `bazel-bin/projects/g99/targets/cc_binary/e2e/g99-e2e-mosaico` (or `-landscape`) with `H2_DESKTOP_STORAGE_ROOT` set to a scratch directory.

## Cases

Cases run in this order, and each starts from rest: Main running on MAIN with Library selected, settled, the screen on and nothing being edited. Before a case the suite recovers to rest with public keys only (restarting Main if it exited); a case that cannot reach rest is ERROR. After a case Main must be back at rest within 5 s, or the case is FAIL.

| Case | Checks |
| --- | --- |
| `startup/boot-animation` | MAIN(Library) appears no earlier than 900 ms after the boot animation starts; DOWN and OK pressed during the animation are dropped, not replayed. |
| `startup/menu-ready` | A fresh Main is RUNNING on its menu with input ready, its settings store readable and the target's screen size within 5 s. |
| `navigation/library-enter-back` | OK enters Library and AI returns to MAIN(Library); DOWN during each transition is ignored. |
| `navigation/settings-enter-back` | Settings enters and AI returns with the main selection restored; entering again starts on the first item. |
| `navigation/non-enterable` | OK on 我的, 应用中心 and 实验室 stays on the main menu. |
| `keys/home-exits-app` | AI leaves the running Orbit app for Library. |
| `keys/screen-toggle` | BOOT turns the screen off and on; a key pressed while off only wakes the screen. |
| `layout/page-geometry` | On MAIN, Library and Settings the wheel and info areas lie inside the screen and do not overlap. |
| `touch/wheel-drag` | A deliberate quarter-screen drag up from the wheel centre turns the main wheel to the next item, and the same drag down turns it back. |
| `touch/wheel-follow` | A held finger turns the wheel with it: a tenth of the screen up turns it about 0.4 items with the selection unchanged while Main draws; back at the start and lifted, the wheel stays put. |
| `touch/wheel-flick` | The quarter-screen drags as 150 ms flicks that Main reads only when it gets to them. Needs `frame-budget`. |
| `touch/wheel-tap` | A tap below the centre turns to the next item, above it back, and on the centre enters Library. |
| `touch/hover-ignored` | A pointer moving over the wheel without contact leaves it still; pressed and dragged afterwards, it turns the wheel one item and back. |
| `touch/empty-frames` | With the CST9220 driver 0.1.1 model, a held finger still turns the wheel with it and a deliberate drag still turns it one item and back. |
| `touch/failed-reads` | The same with the driver 0.1.2 model, whose reads between two reports fail. |
| `perf/idle-frames` | Over 3 s at rest, reports frame timing and touch reads; with `frame-budget` the longest gap between touch reads stays within 50 ms. |
| `perf/rotate-frames` | Reports a key rotation; with `frame-budget` it draws at least five frames and holds the 50 ms / 20 FPS budget. |
| `perf/page-frames` | Reports the first Library and Settings entry/back animations after restarting Main; with `frame-budget` every measured frame holds the 50 ms / 20 FPS budget. |
| `settings/persist-across-restart` | Volume, brightness and screen timeout changed in Settings survive a Main restart; the original values are restored the same way. |
| `wifi/connect-test-network` | Settings › Wi-Fi scans, connects to `HAIVIVI-MFG` with password `!haivivi` and closes the list. Needs `wifi`. |
| `wifi/reconnect-after-restart` | After a Main restart with the link dropped, Main reconnects to the saved network on its own. Needs `wifi`. |

A case whose capability the target does not declare reports UNSUPPORTED. Screen-off pause of a running app is not asserted: the pinned Lua host has no pause event.

## Selection

A selection is a comma-separated list of terms: a group (`navigation`), a case (`navigation/non-enterable`) or a prefix ending in `*` (`keys/screen*`). A leading `-` excludes. Without include terms every case runs. An unknown term fails the run before Main starts.

```sh
bazel test --config=macos_arm64 //projects/g99/targets/cc_test/e2e:mosaico_test --test_env=H2_E2E_SELECT=startup,keys
bazel test --config=macos_arm64 //projects/g99/targets/pkg_tar/web/e2e/mosaico:browser_test --test_arg=--query=select=navigation/non-enterable
bazel build --config=esp32s31 --//projects/g99/apps/e2e:select=startup,keys //projects/g99/targets/h2_loader_tar_zlib/e2e/espressif/mosaico:package
```

## Report

Every line starts with `H2_E2E_` and carries `schema=1 product=g99 suite=g99`. `H2_E2E_START` lists the selection, capabilities, board, target and size. One `H2_E2E_CASE` per selected case gives `status` (PASS, FAIL, ERROR or UNSUPPORTED), `rc`, `duration_ms` and `detail`. Touch and perf cases add `H2_E2E_METRIC` lines before their `H2_E2E_CASE` with what they measured: frames, frame interval and work time in microseconds (with the display draw and present parts), redrawn pixels, touch reads and the longest gap between them. The run ends with one `H2_E2E_SUMMARY`:

```text
H2_E2E_SUMMARY schema=1 product=g99 suite=g99 selected=17 terminal=17 pass=16 fail=0 error=0 unsupported=1 setup_rc=0 cleanup_rc=0 fixture_restore_rc=0 output_rc=0 complete=true first_failure_case=- first_failure_rc=0 exit_code=0 duration_ms=46512 board=g99/mosaico target=desktop
```

`exit_code` is 0 when the run completed without FAIL or ERROR, 1 when a case failed, and 2 when the run could not complete (setup, cleanup, fixture restore or report output failed). The desktop test exits with it; the Web test passes only on `complete=true` and `exit_code=0`.

## State

Main runs with the preference namespace `g99_e2e` instead of its production `g99`, so production settings are never read or written. The suite clears that namespace before the first Main start and again after the last stop. The Wi-Fi cases record whether the test network was saved and connected before they ran and put both back afterwards. On desktop, preferences live under `TEST_TMPDIR`.
