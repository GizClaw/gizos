# Final PAL acceptance — 2026-09-15

Firmware source is `3d7db276`; host source includes `fd8c61d5` and `c3009198`. The five production packages were built together from that source immediately before committing the buffered-poll change. Their frozen hashes and structured results are in [acceptance data](./pal-final-acceptance.json). No diagnostic RX or host instrumentation is included. Earlier package results and diagnostic failures remain in the [historical round](./pal-final-round.md).

## Completed checks

- Native Loader, Display App, crash-before-confirm App, public PAL and audio-system packages build under the requested AC791N OrbStack configuration (113.697 seconds).
- Strict Clang/GCC real-source buffered-poll fixture: twelve failing-before combinations, sixteen passing-after combinations including four partial-frame controls. Existing deadline, handoff and iostream tests pass. The handoff/READY and erase fixtures have their own recorded fail-before runs. The host/CLI regression set and host-only iOS analysis pass.
- Production Loader installation through the running Loader's own upgrade flow passes in 52.693 seconds. Both partitions converge to image `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`, Stage is empty, and `last_result=0`.
- The unchanged full UART lifecycle passes **25/25** in **331.305 seconds**, including install-loader, App/crash rollback and coredump read/erase. `reboot-loader-monitor` passes in **7.821 seconds**. The ordinary fixed CLI previously completed Loader reboot in **3.55 seconds**; the suite timing also includes its configured monitor interval and verification.
- Public PAL runs without tracing. The ordinary CLI monitor captures all ten case lines and `result=0 passed=10 failed=0`: Filesystem suite 64/case 13; Core suite 1/cases 1–6, 10, 11; offline Wi-Fi suite 32/case 27. Every case returns zero. Monitor elapsed time is 32.372 seconds; exit 130 is the deliberate SIGINT after observing the complete aggregate. Independent App identity and return-to-P1 status checks pass.

- Full BLE lifecycle through Terminal.app passes **22/22 twice**, in **371.637** and **366.230 seconds**. Both runs include App installation over BLE, App commands and transfer, Stage-preserving reboots, install-loader, crash rollback and coredump read/erase. The second log contains a CoreBluetooth disconnect notification near the last status; that case and the suite both complete successfully without an external retry.

- Audio-system startup and 30 seconds of streaming pass: READY observed, 23 microphone peak reports, correct running App hash, then a successful return to P1. The monitored install/observation takes 62.534 seconds. This does not establish acoustic quality or controlled stop/restart.

Final independent UART status: UID `d879349abc9f`, active/running P1 Loader `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`, `next_partition=1`, `boot_intent=loader`, `stage_valid=0`, `last_result=0`. P1 and P2 are valid; P2 holds audio App `7714f5006e8020c8f51be7b421ad7692fa181d3b6b917bc171acaa617e5f0111`. No human reset or recovery was needed for this final round. UART and BLE test processes have exited.

## Defects and decisions

- Host text-after-frame omission and reboot-history verification: `fd8c61d5`, [handoff evidence](./pal-host-transport.md). A temporary OPEN-ACK parser discarded bytes it had consumed beyond the ACK. The permanent session now receives every trailing byte. Ordinary reboot treats `last_result` as prior installation history; upgrade verification still requires success.
- Host reset reconnect: `c3009198` retires the old KCP conversation when a standalone READY banner arrives. It ignores timestamped historical SDK log replay. The real-source reset/READY fixture fails before and passes after with Clang/GCC.
- Device buffered-frame progress: `3d7db276`, [root cause and regression](./pal-buffered-poll.md). This separately explains the install-loader stall after a correctly received OPEN ACK. No timeout, retry or deadline was weakened.
- O11: `417dcc98`, [SDK audit and fault injection](./pal-upgrade-erase.md). The pinned updater ignores the erase callback result. Verified erase failures therefore latch and gate all later completion/header publication. Native erase errors, dirty readback, short readback and false SDK-success completion fail closed. Physical erase faults were not injected.
- O4: `90a0fc56`, [cross-provider decision](./pal-event-contract.md). Desktop and ESP disagree. Recommend asynchronous queue admission with owned payloads and safe callback retirement; synchronous admission-bounded dispatch is the alternative. No JieLi-only contract change was made.
- O10 watchdog: `b3ca72c5` follows retained capture `8d1ec24b`. Two App and two Loader watchdog trials previously returned to P1 without human action, with correct origin and valid dumps; [paired trial evidence](./pal-watchdog-recovery.md). The final lifecycle crash cases additionally exercise rollback and coredump persistence, not a replacement for those watchdog-specific trials.

## Evidence and limits

Raw final-package commands, reports and logs are under `tmp/jieli/pal-review-next/final-v2/`: `install.json`, `upgrade.log`, `uart.json`, `uart.log`, and the BLE reports/logs. PAL raw output is `tmp/jieli/pal-review-next/diagnostic-runs/final-v2-pal/1/pal.log`, with `result.json`, `after.status` and `returned.status`. Audio raw output and independent final status are `tmp/jieli/pal-review-next/diagnostic-runs/final-v2-audio/1/pal.log` and `returned.status`. Log/status names are deliberately code spans, not VitePress links.

The current UART layout does not provide physical USB deadline coverage. Connected Wi-Fi/AP traffic, never-completing scan recovery, controlled audio stop/restart, physical touch/button/display fault teardown, power-loss atomicity and physical erase fault injection are not established by this round. ESP/BK buffered-poll changes have host-fixture coverage, not new native/board acceptance. O3 recovery and O4 require decisions; O7 concurrency/DNS and O9 rename/open SDK audits remain open in the [ledger](./pal-review.md).

Host validation logs are `/tmp/jieli-final-host-recheck.log` (12 host/CLI tests), `/tmp/jieli-final-gcc-recheck.log`, and `/tmp/jieli-buffered-{before,gcc-before,after,gcc-after,ios,native}.log`. Final guides validation is `/tmp/jieli-final-acceptance-guides-final.log`; `git diff --check` passes. Commits remain local; the unrelated untracked `lock` file is untouched.
