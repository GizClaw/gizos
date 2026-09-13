# PAL review follow-up hardware — 2026-09-14

## Before the App task-policy correction

UART Loader self-upgrade used the Loader-owned flow only. No USB download, format-all, or other serial port was used. UID `d879349abc9f`, UART `/dev/cu.usbserial-20131240`, 460800 baud.

The rebuilt Loader image `20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71` passed the complete UART lifecycle: **25/25**, 556.037 seconds. [Machine report](./pal-followup-uart-lifecycle.json), [package identities](./pal-followup-artifacts.json). These results precede the subsequent task-policy correction and are not final-source acceptance.

The color-bar App was installed through UART Loader into P2. A Terminal.app BLE status request failed with CoreBluetooth timeout (`rc=-10`). UART capture identifies `stackoverflow` in `$bleikcp/server`, stack limits `40c9ba0..40cb39c` (approximately 6 KiB), followed by an App reboot. Subsequent BLE status cases were not run. Independent UART status still answered with valid P1 Loader and valid P2 App, so the UART safety-stop condition did not occur.

Evidence files under `tmp/jieli/pal-review-followup/`: `ble-app-1.log`, `ble-app-1.exit`, `ble-app-uart-install.log`, `after-ble-failure.status`. These local raw files are named in code spans because VitePress cannot resolve log/status links.

Named SDK tasks use generated task policies, ignoring PAL `min_stack_size`. The synchronous command-loop migration therefore needs the former console's 12288-word budget in the App server policy. The regression now checks the actual policy and SDK dispatch for all four App targets; each fails before the correction. After correction, the fixture passes with macOS Clang and Linux GCC under `-Wall -Wextra -Werror`; the H2Loader package test passes. All seven native package targets (Loader, display, audio-system, touch, button, crash-before-confirm and PAL) build successfully in 138.446 seconds. Hardware validation after this correction is pending.
