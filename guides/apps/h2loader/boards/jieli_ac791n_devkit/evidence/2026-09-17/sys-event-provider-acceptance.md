# System-event provider on SDK sys_event — 2026-09-17

## Outcome and source

**PASS.** The wl82 PAL system-event provider now enqueues every event into the SDK `sys_event` ring and delivers it on the provider-owned `h2_sysevt` task (GizOS Issue #448). With that provider in every image, the board completed Loader self-update, PAL 10/10 with all ten case lines in the first pass, UART lifecycle 25/25, BLE lifecycle 22/22 twice, button and touch trials with confirmation captured, and audio-system READY plus 35 s of streaming, and the final independent status shows the new P1 Loader, Stage empty and `last_result=0`.

Tested source is `origin/main` `e6c7b6aa` plus the single provider commit `99f1f89a` on branch `jieli/wl82-sys-event-provider`; no other source differs. The board is UID `d879349abc9f` on `/dev/cu.usbserial-20131240` at 460800 with BLE endpoint `5:818f070641f0`. Before the round the board ran the 2026-09-16 Loader `28123452b282b325471e8f64fa5d462d314c63c934bf6fea66e6e3936bd3258f` in Partition 1 with an empty Stage and the button App `6d6975091d9dceff5c1a7da80c69e90bc01dfefd1466fd60525bcf99e2a743a8` in Partition 2. Every install went through the UART Loader into Partition 2; the new Loader reached Partition 1 only through its own trial and copy-back; there was no USB download, format or manual power cycle, and no reset left status silent.

## Ordered results

| Step | Result | Result codes and coverage |
| --- | --- | --- |
| 1: build | PASS | Seven native packages in OrbStack, exit 0, 166.671 s (484 cached actions, 21 executed); host CLI and runner built with `--config=macos_arm64`. |
| 2: Loader self-update | PASS | `send --file` rc=0 in 32.9 s; `reboot upgrade --monitor` captured `H2_JIELI_LOADER_TRIAL confirmed=1 publish_gate=before-copy-p1` and `H2_JIELI_LOADER_HEADER published=1 confirmed=1`; independent status: `running_partition=1`, both partition image checksums `ed7d71a6…`, `stage_valid=0`, `last_result=0`. |
| 3: PAL | PASS | Ten distinct case lines before the first aggregate, all `result=0`; aggregate `H2_PAL_E2E result=0 passed=10 failed=0`; P2 image `1a0fd02b…`; explicit `reboot loader` returned to P1. |
| 4: UART lifecycle | PASS | Detached run: `H2_LOADER_E2E result=PASS rc=0 cases=25 passed=25 failed=0 elapsed_ms=339512`, process exit 0. |
| 5: BLE lifecycle twice | PASS | Terminal.app runs: `cases=22 passed=22 failed=0 elapsed_ms=373098` then `cases=22 passed=22 failed=0 elapsed_ms=382811`, both process exit 0. |
| 6: button trial | PASS | Monitored relaunch captured `input-start result=0`, `H2_JIELI_BUTTON_SMOKE_READY buttons=8 display=480x320 result=0`, `JIELI_APP_CONFIRM result=OK code=0 target=0 transport=0`, `JIELI_TRIAL_TIMER state=deleted id=11`; status `running_partition=2`, `stage_valid=0`, `last_result=0`, P2 image `7b183526…`; `reboot loader` returned to P1. |
| 7: touch trial | PASS | Captured `H2_JIELI_TOUCH_SMOKE stage=input-start result=0`, `H2_JIELI_TOUCH_SMOKE_READY touch=ft6236 display=480x320 result=0`, `JIELI_APP_CONFIRM result=OK code=0 target=0 transport=0`, `JIELI_TRIAL_TIMER state=deleted id=11`; status P2 confirmed, `stage_valid=0`, `last_result=0`, P2 image `0c0d8afd…`; returned to P1. |
| 8: audio-system | PASS | Install run captured `H2_JIELI_AUDIO_SYSTEM stage=run result=0`, `H2_JIELI_AUDIO_SYSTEM_READY mic=1 speaker=1 aec=dac-software-ref` and `JIELI_APP_CONFIRM result=OK`; a second `reboot app --monitor` relaunch reached READY within 10 s and produced 27 `H2_SMOKE_AUDIO_MIC peak=…` reports during the following 35 s; status P2 confirmed, `stage_valid=0`, `last_result=0`. |
| 9: final independent state | PASS | `running_partition=1`, `next_partition=1`, `active_checksum=ed7d71a667fe5ef20d09afff11f4909ba33a13af7e852ec4f0ac981541290fdf`, `stage_valid=0`, `last_result=0`, P2 holds the audio-system App `5874b419…`. |

## Builds and immutable artifacts

The native build ran in OrbStack `embed-zig-noble-amd64` from this worktree after sourcing `/Users/idy/h2vivi/firmwares-devenv/export.sh` and unsetting `IDF_PATH H2LOADER_IDF_PATH IDF_PYTHON_ENV_PATH IDF_TOOLS_PATH`, with `bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build --config=ac791n --symlink_prefix=bazel-amd64-` on the loader, pal, button, touch, audio-system, display and crash-before-confirm package targets; the seven outputs were copied out of the VM immediately and hashed together with their `app/jieli/update.ufw` images. All packages target `jieli_ac791n_devkit` / `wl82`; version strings are `bazel-native-artifacts`, so SHA-256 is the identity.

| Artifact | Package SHA-256 | `app/jieli/update.ufw` SHA-256 | Package / image bytes |
| --- | --- | --- | --- |
| loader | `19df4f8ef4a8a39242459147f3345aec4cad240862ba0a598970c0dd58b1ccda` | `ed7d71a667fe5ef20d09afff11f4909ba33a13af7e852ec4f0ac981541290fdf` | 928798 / 939629 |
| pal | `8407de4baa444dbb5bd9962ee8d053f03f3609d94ae040fa60f1a77b3dac9705` | `1a0fd02bc77bfc85c9370992a23f1b9ea7d8e8ec6b9cfdceb56111450ca6da2c` | 891924 / 903117 |
| button | `d4c26d87060b55a9b287b7b67b50a638a63acf9ba1cb17c7a0d372ebc9444f95` | `7b183526dec8e872011b6f1c3b8428632392adee62da4473239787d4726352ac` | 1279378 / 1293745 |
| touch | `6bfc517687509605df378767de7bbd6d4cea902217b3ebaf6ef36c7c960ba655` | `0c0d8afda62f274ec440caa0a31b3874cd713cd2d272dbe407e7e3d50c52ee3d` | 1280003 / 1294321 |
| audio-system | `ff82f0076b14cdfc84e106730c38d6b1d2224ef236d961030121b864f98fb1cb` | `5874b4190a6d1be753bc81b503a25fd170794bc48e631fa5adf7118f8f8cc071` | 1079659 / 989025 |
| display (color-bar) | `14dc0d74343a71a84bdca41c2f296354e44e6aed1c18d3c79b09c8340af2bdff` | `ff4c95adf533f81b4c7fa270b690eeab6c47013544a758b3243dfe6e9440cd2c` | 866660 / 877537 |
| crash-before-confirm | `9f1eaab2ef5ba5a216804ec32f639854d959345f25ad2931c58cfe5ca87a374a` | `5330dc718a849059834dfb99399d153449d3454f46ca50d945da92d80a00e978` | 897593 / 908513 |

Host command: `bazel build --config=macos_arm64 //projects/h2loader/targets/cc_binary/cli:h2loader //projects/h2loader/targets/cc_binary/e2e-runner:e2e-runner`. CLI SHA-256 is `c6347eb372f1d881d6956c7876033da8a7c46c296729edd14faea2a59846c0d5`; runner SHA-256 is `69dbb7040a4c9a025344ba3d6422e20289f275220bad4d80416b64d8926a832b`. A copy of the runner outside `bazel-bin` aborted at load with `Library not loaded: @rpath/libavcodec.62.dylib` before touching the board, so the suites ran the same binary in place.

## Lifecycle suites

The UART suite was launched detached with `--uart /dev/cu.usbserial-20131240 --baud 460800 --expected-board jieli_ac791n_devkit --expected-target wl82`, the color-bar App, Loader and crash-before-confirm packages, `--monitor-ms 3000` and a JSON report outside the worktree; all 25 cases passed with rc=0, including `install-loader` (61020 ms), `install-crash-app` (60100 ms), `reboot-upgrade-monitor` (60574 ms), the two preserve-stage reboots (34505 ms and 34390 ms) and the four coredump cases.

Both BLE suites were launched from Terminal.app with `--ble-id 5:818f070641f0` and the same three packages; `--monitor-ms` is UART-only and was omitted after one immediate usage exit (exit 2, no board contact). Run 1 passed 22/22 in 373.098 s with `install-app` 57648 ms, `install-loader` 63803 ms and `install-crash-app` 64627 ms; run 2 passed 22/22 in 382.811 s with `install-app` 62740 ms, `install-loader` 58576 ms and `install-crash-app` 67046 ms. No case failed in either run.

## Boundaries

This round establishes that the sys_event-based provider carries the Loader's BLE service events and the runtime's input events on this one board through the standard lifecycle, trial and smoke suites; it does not measure queue overflow, the SDK 40 s handler timeout, interrupt-context posts or long-duration RF stability on hardware, which remain covered only by host tests and design analysis. All resets were software resets through Loader lifecycle commands, and monitors were stopped with SIGINT (exit 130) after the required lines were captured, so monitor exit codes are not board-health facts.
