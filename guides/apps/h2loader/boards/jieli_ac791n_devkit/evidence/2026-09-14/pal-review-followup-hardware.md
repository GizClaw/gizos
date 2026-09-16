# PAL review follow-up hardware — 2026-09-14

## Before the App task-policy correction

UART Loader self-upgrade used the Loader-owned flow only. No USB download, format-all, or other serial port was used. UID `d879349abc9f`, UART `/dev/cu.usbserial-20131240`, 460800 baud.

The rebuilt Loader image `20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71` passed the complete UART lifecycle: **25/25**, 556.037 seconds. [Machine report](./pal-followup-uart-lifecycle.json), [package identities](./pal-followup-artifacts.json). These results precede the subsequent task-policy correction and are not final-source acceptance.

The color-bar App was installed through UART Loader into P2. A Terminal.app BLE status request failed with CoreBluetooth timeout (`rc=-10`). UART capture identifies `stackoverflow` in `$bleikcp/server`, stack limits `40c9ba0..40cb39c` (approximately 6 KiB), followed by an App reboot. Subsequent BLE status cases were not run. Independent UART status still answered with valid P1 Loader and valid P2 App, so the UART safety-stop condition did not occur.

Evidence files under `tmp/jieli/pal-review-followup/`: `ble-app-1.log`, `ble-app-1.exit`, `ble-app-uart-install.log`, `after-ble-failure.status`. These local raw files are named in code spans because VitePress cannot resolve log/status links.

Selected original UART evidence (`ble-app-uart-install.log`, lines 954–963):

```text
H2_JIELI_WARM_LOG [00:00:31.001]stackoverflow
H2_JIELI_WARM_LOG [00:00:31.001]current_task : $bleikcp/server
H2_JIELI_WARM_LOG [00:00:31.001]usp limit 40c9ba0  40cb39c
H2_JIELI_WARM_LOG [00:00:31.001]exception reason : c0_pc_limit_err_r
```

Named SDK tasks use generated task policies, ignoring PAL `min_stack_size`. The synchronous command-loop migration therefore needs the former console's 12288-word budget in the App server policy. The regression now checks the actual policy and SDK dispatch for all four App targets; each fails before the correction. After correction, the fixture passes with macOS Clang and Linux GCC under `-Wall -Wextra -Werror`; the H2Loader package test passes. All seven native package targets (Loader, display, audio-system, touch, button, crash-before-confirm and PAL) build successfully in 138.446 seconds. Hardware validation after this correction is pending.

## After the App task-policy correction (`8c235666`)

The corrected color-bar App was installed through UART Loader into P2. Three independent BLE status commands, launched through Terminal.app, all returned 0 with UID `d879349abc9f` and App image `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05`. No stack overflow appears in the simultaneous UART capture. [Status results](./pal-policy-fix-ble-app-status.json), [exact package identities](./pal-policy-fix-artifacts.json). Raw evidence: `policy-ble-app-1.log` through `policy-ble-app-3.log`, their `.exit` files, and `policy-app-install.log` under the same local directory.

The full BLE lifecycle suite is **not run**: its normal cases install Apps through BLE, conflicting with this task's explicit UART-only App-install constraint. Clarification was requested and remains unanswered. No Bluetooth permission failure occurred. Three read-only App sessions are not a substitute for the full 22-case suite.

## Public PAL E2E remains blocked

[Extracted run results](./pal-followup-native-stalls.json).

The unchanged PAL package (`bf635cc1…` image in the identities file) passed Filesystem case 13, then stopped advancing inside Core. It produced no aggregate report and never reached offline Wi-Fi case 27. Independent UART status continued to respond. Raw evidence: `policy-pal-e2e.log`, `policy-pal-core-stall.status`.

Two diagnostic-only PAL builds mirrored existing case progress and then sync calls to UART while preserving the original providers, cases and timeouts. The first identified successful Core cases 1–6 followed by a stall in condition case 10. The second passed case 10 but later stopped advancing inside concurrency; added tracing changes scheduling and does not establish a fix. Each diagnostic App was installed only through UART Loader, and independent status remained available after each stall.

These temporary source changes were saved as `pal-sync-diagnostics.patch` and removed from the worktree. Diagnostic evidence: `pal-trace-e2e.log`, `pal-trace-after-stall.status`, `pal-sync-trace-e2e.log`, `pal-sync-trace-after-stall.status`. Neither diagnostic run counts as PAL acceptance. Root cause is not established; host pthread/TSan evidence does not close this native SDK scheduling/synchronization failure. No timeout, retry, assertion or test selection was weakened.

## Final-source UART lifecycle and board state

The policy-fix Loader was installed through its own upgrade flow, with startup event 4 returning 0. Independent UART status verified image `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`, empty Stage and `last_result=0` before the suite.

The unchanged full UART runner passed **25/25**, **551.149 seconds**, `rc=0`: [machine report](./pal-policy-fix-uart-lifecycle.json). It used the Loader, display and crash packages in [policy-fix identities](./pal-policy-fix-artifacts.json). The suite exported 2096 coredump bytes, erased them and verified zero bytes afterwards. Raw files: `policy-loader-send.log`, `policy-loader-install.log`, `policy-loader-after-install.status`, `policy-uart-lifecycle.log`, `final-board.status`.

[Final independent UART status](./pal-followup-final-board.json): UID `d879349abc9f`, active Loader, P1 running/next, `boot_intent=auto`, `last_result=0`, P1 image `6b47889e…`. P2 and Stage retain the crash-before-confirm App (`a3226f1c…`), rejected for boot by the successful rollback case; valid partition metadata is not proof that this deliberately crashing App is bootable. No post-suite installation changed that fixture, and no UART reader was left running. A final independent BLE status via Terminal.app also returned 0 with identical UID, image and partition fields (`final-loader-ble.status`, `final-loader-ble.exit`); it is a read-only smoke check, not the full lifecycle suite.

Firmware push acceptance remains **blocked**: public PAL Core did not complete and Wi-Fi 27 was not reached; the full BLE lifecycle is not run. UART 25/25 and BLE App status 3/3 do not override those gates.
