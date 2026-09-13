# 2026-09-14 — current-source Loader BLE lifecycle, two runs

**Both full BLE-only lifecycle runs passed: 22/22 in 385.850 s, then 22/22
in 357.072 s (rc=0 each).** UID `d879349abc9f`, freshly discovered BLE endpoint
`5:818f070641f0`. UART observed 34 and 33 successful connections respectively,
with zero `TO - ll_conn_supervision` and zero `LL_REJECT` in either capture.
No production code, runner, tests, timeouts, or retry settings were changed.

Worktree head before testing: `049f8fa14eaa7ed8238dec914dcc872b19edcfbd`.
The packages were built from `2a814d324455a9f2995c3b4348e82bb300bb77b7`;
`git diff 2a814d32..049f8fa1 --stat` contains documentation/evidence only.
This reuses the installed current-source Loader and the same host binaries
and three packages as the [UART lifecycle](./loader-uart-lifecycle.md).
No native rebuild or new host tests were needed because no code changed;
the earlier native build and host tests are recorded in that evidence.

## Discovery, commands, and capture

Re-listed `/dev/cu.*` and checked `lsof /dev/cu.usbserial-20131240` before use:
no stale reader. Only this UART was opened, at 460800 baud. No manual reset,
USB download/recovery, format, other-port access, or system/TCC change occurred.
BLE host commands were all launched through Terminal.app via AppleScript.
The initial attempted `--no-uart scan` exited 2 with usage (unsupported option,
no hardware test); the corrected BLE-only scan below exited 0. It does not
probe serial ports. The advertisement board hash `fnv1a64:081d2f80ed6330aa`
matches `jieli_ac791n_devkit`; an independent BLE status then verified UID,
P1 image and complete partition metadata against UART before testing.

```sh
bazel-bin/projects/h2loader/targets/cc_binary/cli/h2loader --transport bleikcp scan
bazel-bin/projects/h2loader/targets/cc_binary/cli/h2loader \
  --transport bleikcp --port 5:818f070641f0 status
```

Each run used the following command, replacing `<N>` with `1` then `2`:

```sh
bazel-bin/projects/h2loader/targets/cc_binary/e2e-runner/e2e-runner \
  --ble-id 5:818f070641f0 \
  --expected-board jieli_ac791n_devkit --expected-target wl82 \
  --app-firmware tmp/jieli/2026-09-14-loader/jieli_ac791n_devkit-color-bar-wl82.update.tar.zlib \
  --loader-firmware tmp/jieli/2026-09-14-loader/jieli_ac791n_devkit-loader-wl82.update.tar.zlib \
  --crash-firmware tmp/jieli/2026-09-14-loader/jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib \
  --report /Users/idy/GizClaw/gizos/.claude/worktrees/qrcode-library-selection-fab623/tmp/jieli/codex-ble/run-<N>.json
```

Terminal launch envelope (absolute paths substituted in the actual invocation):

```sh
osascript -e 'tell application "Terminal" to do script "cd <worktree> && <command> > <absolute-host.log> 2>&1; echo EXIT=$? >> <absolute-host.log>"'
```

The BLE-only suite has 22 cases. `--monitor-ms` enables UART-only monitor cases
and requires the runner to own UART; it is intentionally omitted, as in the
2026-09-13 BLE-only reports. No lifecycle cases were disabled. The independent
UART reader ran throughout each BLE suite. It used `/usr/bin/python3` with
pyserial 3.5: `Serial(port=None, baudrate=460800, timeout=0.25)`, DTR/RTS set
false before opening `/dev/cu.usbserial-20131240`, binary unbuffered file output,
no writes. It was stopped with SIGTERM after the host `EXIT=0`, closing the
port before each independent status command:

```sh
bazel-bin/projects/h2loader/targets/cc_binary/cli/h2loader \
  --no-ble --port /dev/cu.usbserial-20131240 --baud 460800 status
```

Local full logs, exact Terminal commands and capture/analysis scripts remain
under `tmp/jieli/codex-ble/`: `scan.log`, `ble-identity.log`,
`run-1-command.txt`, `run-2-command.txt`, `run-1-host.log`, `run-2-host.log`,
`run-1-uart.log`, `run-2-uart.log`, `capture.py`, and `summarize.py`.
Committed selected UART traces `loader-ble-run-1-uart-trace.log` and
`loader-ble-run-2-uart-trace.log` preserve original line numbers and ordered
connection/disconnect, reboot, update and rollback markers. They are selected
traces, not substitutes for the full local captures.

## Artifacts and independent status

All package bytes/SHA-256s were freshly verified and match
[package and image identities](./loader-artifacts.json):

| Package | Bytes | SHA-256 |
| --- | ---: | --- |
| `jieli_ac791n_devkit-color-bar-wl82.update.tar.zlib` | 856298 | `354ad51fe0520caf0dd2508a1cdfe137f18191f3ddee6db6f5b6879a4ab1c2d9` |
| `jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib` | 887289 | `a6fb82b90e15d34c1889b7c8802c7f08957081f1efc71f7f29bdb10c5d83483b` |
| `jieli_ac791n_devkit-loader-wl82.update.tar.zlib` | 918365 | `9b84d5fdf10b69c56bace2f3bbb4b9b4594526d0f62789f1d1fd5e1bfd0612ca` |

`loader-ble-before.status`, `loader-ble-after-run-1.status`, and
`loader-ble-after-run-2.status` are independent UART reads and byte-identical:
UID `d879349abc9f`, active Loader, P1 running/next, boot_intent=auto,
last_result=0, both partition metadata valid.

| Location | Package SHA-256 | Image SHA-256 |
| --- | --- | --- |
| P1 | `9b84d5fdf10b69c56bace2f3bbb4b9b4594526d0f62789f1d1fd5e1bfd0612ca` | `42c39b7aae00917e44e9807503fe57fdb4ccad25b58793ab12087a2da03d22cd` |
| P2 and Stage | `a6fb82b90e15d34c1889b7c8802c7f08957081f1efc71f7f29bdb10c5d83483b` | `a75bdeca184ecc78ebdf5126dfc75df3170976dbe4a4fcfddfa7da363c3ac93e` |

P2 and Stage retain the unconfirmed crash App, which is rejected for boot;
valid P2 metadata does not mean a healthy runnable App. Both runs observed
`H2_CRASH_BEFORE_CONFIRM_READY action=crash` and subsequent
`H2_JIELI_TRIAL_ROLLBACK app_bootable=0 action=command-mode`, verified/exported
2096 coredump bytes, erased the dump and confirmed zero bytes afterwards.
No post-suite installation or cleanup changed these fixtures.

Each `install-loader` is a same-image install: the package was already in P1.
Both UART traces show events 1 and 4; the case's JSON status confirms identical
valid Loader images in P1/P2, empty Stage and last_result=0. This is BLE normal
installation coverage, not a new distinct-image trial/confirm/copy proof.
Today's earlier UART self-update provides the latter evidence. P1 was never
written outside the Loader's normal install flow.

## Per-case results

Unmodified runner reports: [run 1](./loader-ble-lifecycle-run-1.json) and
[run 2](./loader-ble-lifecycle-run-2.json). All rows are PASS with rc=0 in both runs.

| Case | Run 1 result | Run 1 ms | Run 2 result | Run 2 ms |
| --- | --- | ---: | --- | ---: |
| help | PASS | 2808 | PASS | 2194 |
| status | PASS | 2296 | PASS | 2103 |
| stats | PASS | 6718 | PASS | 2326 |
| legacy-commands-absent | PASS | 3451 | PASS | 2400 |
| send | PASS | 33396 | PASS | 29768 |
| stage-abort-after-send | PASS | 5034 | PASS | 2412 |
| install-app | PASS | 56819 | PASS | 55343 |
| app-help | PASS | 2517 | PASS | 2628 |
| app-status | PASS | 2624 | PASS | 3238 |
| app-stats | PASS | 3058 | PASS | 3180 |
| app-memory | PASS | 2359 | PASS | 2265 |
| app-legacy-commands-absent | PASS | 2138 | PASS | 3829 |
| app-send | PASS | 29198 | PASS | 32269 |
| app-stage-abort-after-send | PASS | 3437 | PASS | 2737 |
| reboot-app-preserves-stage | PASS | 40198 | PASS | 40262 |
| reboot-loader-preserves-stage | PASS | 36524 | PASS | 38000 |
| install-loader | PASS | 54869 | PASS | 54339 |
| install-crash-app | PASS | 84554 | PASS | 63502 |
| coredump-status | PASS | 2821 | PASS | 2158 |
| coredump-dump | PASS | 2210 | PASS | 5023 |
| coredump-erase | PASS | 4425 | PASS | 4109 |
| coredump-status-after-erase | PASS | 4201 | PASS | 2987 |

## UART connection health and limits

Per-connection counts, line ranges, disconnect reasons and full raw UART
SHA-256s: [run 1 connections](./loader-ble-run-1-uart-connections.json),
[run 2 connections](./loader-ble-run-2-uart-connections.json).
A connection window begins at each `H2_JIELI_BLE_CONNECT subevent=` and ends
before the next such marker (or capture end). Every individual window has
zero supervision timeout and zero LL reject; these are not merely suite totals.

| Observation | Run 1 | Run 2 |
| --- | ---: | ---: |
| Successful connection markers | 34 | 33 |
| Supervision timeout drops | 0 | 0 |
| LL reject messages | 0 | 0 |
| SDK `conn nack` messages | 1835 | 1746 |
| Unsupported connection-param-request messages | 32 | 32 |
| Disconnect reason 19 | 23 | 25 |
| Disconnect reason 22 | 9 | 6 |

Reason 19 (`0x13`, remote user terminated) and 22 (`0x16`, local host terminated)
are the only reported disconnect reasons. These explicit disconnects must not
be counted as supervision-timeout drops. Each run also has two connections
with no disconnect callback before a logged software reboot during the install
flows; the per-connection JSON retains those windows rather than inventing a
reason. No connection failure status was observed.

The SDK still logs `conn nack`, `UNSUPPORT LL_CONNECTION_PARAM_REQ`, advertising
already-enabled diagnostics, and `_____delay_____`. Counts above are log-message
counts, not a measured RF packet-loss rate. No claim is made that these
nonfatal diagnostics are fixed or that the radio is error-free. Two passing
runs do not root-cause or eliminate the intermittent failure reported on
2026-09-13, nor the earlier UART reconnect latency. No speculative fix was made.

This verifies two consecutive BLE lifecycle runs on this device, Loader and
host setup with UART observation; it does not prove long-duration RF stability,
new power-cut coverage, or broad PAL acceptance. The existing RF-sleep/PHY/DLE/
MTU policy was unchanged. Terminal had working Bluetooth permission throughout.

## Documentation validation

`bazel build --config=macos_arm64 //guides:build` and `git diff --check`
were run before the local evidence commit. No push, remote operation, rebase,
or amend was performed. Final UART ownership check found no reader.
