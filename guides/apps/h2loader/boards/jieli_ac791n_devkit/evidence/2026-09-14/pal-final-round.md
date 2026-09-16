# Initial acceptance round — 2026-09-14

**Historical round before the [buffered-poll correction](./pal-buffered-poll.md): UART acceptance was incomplete because the managed install-loader session stalled after board convergence.** This is not a board blackout. Independent status passes after terminating the stale host reader, with P1 active, both Loader slots valid, empty Stage and `last_result=0`. No physical reset, USB download, formatting or recovery was used in this round.

Firmware packages were frozen from the O11 production source (`417dcc98`; subsequent changes through `c3009198` affect host tools and documentation). The tested Loader image is `224b3b84a278d65677d5fa9bfae7ab585007ef89079e62084a3e4cffb676e3f9`, package `3a75f4ad6d1f9f2a054615ac608bc6d5efd53bc4778f37e4f4bb01467fd3a37d`. Exact package hashes and sizes are in `tmp/jieli/pal-review-next/final-acceptance/packages.json`. A later rebuild of restored source produces a different Loader package; it is not substituted for the frozen packages under test.

[Structured results](./pal-final-round.json).

## Confirmed results

| Check | Result | Raw evidence |
| --- | --- | --- |
| Distinct-source Loader install | Passed through the Loader's upgrade flow; P1/P2 converge, Stage clears | `final-acceptance/loader-result.json`, `loader-upgrade.log`, `loader-after.status`; [O11 record](./pal-upgrade-erase.md) |
| Ordinary reboot-loader-monitor | Passed in 7,842 ms including the 3,000 ms monitor window | `final-acceptance/install-session-stalled-uart.log`; [host repair](./pal-host-transport.md) |
| Ordinary PAL monitor on final host | All ten cases captured, all results zero: Filesystem 13, Core 1–6/10/11, offline Wi-Fi 27; aggregate 10 passed, zero failed | `diagnostic-runs/final-c300/1/pal.log`, `result.json`, `after.status`, `returned.status` |
| BLE lifecycle 1, Terminal.app | 22/22 passed, 374,756 ms; includes Loader install, crash rollback and coredump download/erase | `final-acceptance/ble-1.json`, `ble-1.log`, `ble-1.exit` |
| BLE lifecycle 2, Terminal.app | 22/22 passed, 374,676 ms on the same packages | `final-acceptance/ble-2.json`, `ble-2.log`, `ble-2.exit` |
| Audio-system smoke | READY plus 30 seconds of streaming, 25 microphone reports; App identity and return to P1 pass | `diagnostic-runs/final-audio-c300/1/pal.log`, `result.json`, `after.status`, `returned.status` |
| Host regression checks | 12/12 host/CLI/runner targets; strict GCC handoff fixture; host-only iOS analysis | `/tmp/jieli-final-host-recheck.log`, `/tmp/jieli-final-gcc-recheck.log`, `/tmp/jieli-final-ios-recheck.log` |
| Native packages | Loader, Display, crash, PAL and audio-system build from restored production source | `/tmp/jieli-final-native-recheck.log` (38.026 s) |

All relative raw paths above are under `tmp/jieli/pal-review-next/`. The PAL run uses the checked-in PAL diagnostics and adds no synchronization tracing. Its monitor was deliberately interrupted after observing all ten cases and the aggregate; exit 130 is the monitor cancellation, not a test failure. The test App identity and returned P1 identity were checked independently.

## Outstanding UART session investigation

The full UART suite passed ordinary commands, reboot monitors, App commands and Stage-preserving reboot cases, then stalled reading status after install-loader. It was interrupted for independent status verification, so no full-suite PASS is claimed. Earlier reset-detection attempts and their corrections remain in the host evidence record.

A targeted reinstall with both slots already containing the Loader passed in 33,421 ms. A targeted App-to-Loader transition reproduced the stall: the host receives the matching OPEN ACK after READY and creates a new conversation, then retransmits the KCP status request. The peer replies with session ACK control frames instead of KCP ACKs. Instrumentation at the POSIX serial write boundary confirms correctly encoded DATA frames, flags zero, containing `h2loader status\n`. Independent status after terminating each diagnostic reader passes promptly (0.59–0.76 s in the measured runs).

Raw diagnostics: `install-only-diagnostic.*`, `app-loader-session-diagnostic.*`, `kcp-wire-diagnostic.*`, `posix-wire-diagnostic.*`, and their independent `.status` files under `final-acceptance/`. The targeted runners are diagnostics, not substitutes for the unchanged full lifecycle suite. No timeout or retry count was changed in production or acceptance tests.

The subsequent device RX trace established that the SDK delivered correct DATA bytes; complete frames were stranded in the parser after a callback deadline. The [correction and regression](./pal-buffered-poll.md) supersede the earlier SDK RX-state hypothesis. The pinned SDK receive IR advances its software read pointer and decrements the receive count; the ISR adds `HRXCNT`. The linked `memmove` has overlap handling, and the native filter layout matches its library layout. Temporary host instrumentation was restored. A separately built RX diagnostic Loader was installed only through install-loader, used to isolate the cause, then replaced by the production correction. It is not a production package.

O4 remains a [cross-platform contract decision](./pal-event-contract.md). Scan completion that never arrives remains a separate O3 SDK recovery decision. Audio stop/restart fault paths, physical USB deadline behavior and destructive erase fault injection are not established by these lifecycle passes.
