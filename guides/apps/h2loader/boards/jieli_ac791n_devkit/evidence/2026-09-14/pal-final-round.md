# Initial acceptance round — 2026-09-14

**Historical round before the [buffered-poll correction](../2026-09-15/pal-buffered-poll.md): UART acceptance was incomplete because the managed install-loader session stalled after board convergence.** This is not a board blackout. Independent status passes after terminating the stale host reader, with P1 active, both Loader slots valid, empty Stage and `last_result=0`. No physical reset, USB download, formatting or recovery was used in this round.

Firmware packages were frozen from the O11 production source (`417dcc98`; subsequent changes through `c3009198` affect host tools and documentation). The tested Loader image is `224b3b84a278d65677d5fa9bfae7ab585007ef89079e62084a3e4cffb676e3f9`, package `3a75f4ad6d1f9f2a054615ac608bc6d5efd53bc4778f37e4f4bb01467fd3a37d`. Exact package hashes and sizes are in `tmp/jieli/pal-review-next/final-acceptance/packages.json`. A later rebuild of restored source produces a different Loader package; it is not substituted for the frozen packages under test.

[Structured results](pal-final-round.md#retained-acceptance-facts-pal-final-round).

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

The subsequent device RX trace established that the SDK delivered correct DATA bytes; complete frames were stranded in the parser after a callback deadline. The [correction and regression](../2026-09-15/pal-buffered-poll.md) supersede the earlier SDK RX-state hypothesis. The pinned SDK receive IR advances its software read pointer and decrements the receive count; the ISR adds `HRXCNT`. The linked `memmove` has overlap handling, and the native filter layout matches its library layout. Temporary host instrumentation was restored. A separately built RX diagnostic Loader was installed only through install-loader, used to isolate the cause, then replaced by the production correction. It is not a production package.

O4 remains a [cross-platform contract decision](./pal-event-contract.md). Scan completion that never arrives remains a separate O3 SDK recovery decision. Audio stop/restart fault paths, physical USB deadline behavior and destructive erase fault injection are not established by these lifecycle passes.

## Retained acceptance facts: pal-final-round

Historical run summary transcribed from `pal-final-round.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| firmware_source | `417dcc98 (later through c3009198 host/docs only)` |
| host_source | `c3009198` |
| uid | `d879349abc9f` |
| packages.audio.tar.package_sha256 | `20eb9bfcebfe5de291dfa7f8a05f88d834fd2673c81736494f83bdad88db1ec6` |
| packages.audio.tar.package_bytes | `1076586` |
| packages.audio.tar.image_sha256 | `f9c07c1e691836a2f59172107cbcf8310690bbb2c8b12f07c5980831290aec49` |
| packages.audio.tar.image_bytes | `986141` |
| packages.crash.tar.package_sha256 | `4819ea59881baba6fe293294c85b6d1d82074d1a2501ea882987f67c6a90c658` |
| packages.crash.tar.package_bytes | `894831` |
| packages.crash.tar.image_sha256 | `b20b8c873ac3b89a4486ad2c03af51149248ea7edf5a07e3a4a00dde1ced7077` |
| packages.crash.tar.image_bytes | `905821` |
| packages.display.tar.package_sha256 | `9326ffca36704d7d2d82d39714d40c37eaec69b1f1f42eb8c52575c0dfc1e2c9` |
| packages.display.tar.package_bytes | `863724` |
| packages.display.tar.image_sha256 | `27e24ac9da9e268be70a5b998f8597fe11c23ba3702036a5ba6187f91a197990` |
| packages.display.tar.image_bytes | `874845` |
| packages.loader.tar.package_sha256 | `3a75f4ad6d1f9f2a054615ac608bc6d5efd53bc4778f37e4f4bb01467fd3a37d` |
| packages.loader.tar.package_bytes | `925986` |
| packages.loader.tar.image_sha256 | `224b3b84a278d65677d5fa9bfae7ab585007ef89079e62084a3e4cffb676e3f9` |
| packages.loader.tar.image_bytes | `937001` |
| packages.pal.tar.package_sha256 | `d6b1892a11b49d4c4d61cb844e728a2a02152a9a3bc7bf1e522a2e6726d84fc7` |
| packages.pal.tar.package_bytes | `888849` |
| packages.pal.tar.image_sha256 | `dc33f602eb38f8355e26e0e8838c88b8f2d6ab6b00dada5835cbbf30692d034b` |
| packages.pal.tar.image_bytes | `900009` |
| pal[1].case_count | `10` |
| pal[1].case_results | `0=10` |
| pal[1].variant | `final-c300` |
| pal[1].iteration | `1` |
| pal[1].outcome | `aggregate` |
| pal[1].monitor_rc | `130` |
| pal[1].elapsed_s | `52.62125637498684` |
| pal[1].package_sha256 | `d6b1892a11b49d4c4d61cb844e728a2a02152a9a3bc7bf1e522a2e6726d84fc7` |
| pal[1].image_sha256 | `dc33f602eb38f8355e26e0e8838c88b8f2d6ab6b00dada5835cbbf30692d034b` |
| pal[1].core_pass | `True` |
| pal[1].aggregate[1][1] | `0` |
| pal[1].aggregate[1][2] | `10` |
| pal[1].aggregate[1][3] | `0` |
| audio[1].case_count | `0` |
| audio[1].variant | `final-audio-c300` |
| audio[1].iteration | `1` |
| audio[1].outcome | `audio-ready-observed-30s` |
| audio[1].monitor_rc | `130` |
| audio[1].elapsed_s | `62.339340000005905` |
| audio[1].core_elapsed_s | `30.981918500037864` |
| audio[1].package_sha256 | `20eb9bfcebfe5de291dfa7f8a05f88d834fd2673c81736494f83bdad88db1ec6` |
| audio[1].image_sha256 | `f9c07c1e691836a2f59172107cbcf8310690bbb2c8b12f07c5980831290aec49` |
| audio[1].audio_ready | `True` |
| audio[1].mic_reports | `25` |
| ble[1].case_count | `22` |
| ble[1].case_results | `PASS=22` |
| ble[1].result | `PASS` |
| ble[1].rc | `0` |
| ble[1].uart_baud_rate | `460800` |
| ble[1].ble_endpoint | `5:818f070641f0` |
| ble[1].repeat | `1` |
| ble[1].monitor_duration_ms | `0` |
| ble[1].app_firmware.bytes | `863724` |
| ble[1].app_firmware.sha256 | `9326ffca36704d7d2d82d39714d40c37eaec69b1f1f42eb8c52575c0dfc1e2c9` |
| ble[1].loader_firmware.bytes | `925986` |
| ble[1].loader_firmware.sha256 | `3a75f4ad6d1f9f2a054615ac608bc6d5efd53bc4778f37e4f4bb01467fd3a37d` |
| ble[1].crash_firmware.bytes | `894831` |
| ble[1].crash_firmware.sha256 | `4819ea59881baba6fe293294c85b6d1d82074d1a2501ea882987f67c6a90c658` |
| ble[1].firmware_url.bytes | `0` |
| ble[1].coredump.expected_bytes | `2096` |
| ble[1].summary.cases | `22` |
| ble[1].summary.passed | `22` |
| ble[1].summary.failed | `0` |
| ble[1].summary.elapsed_ms | `374756` |
| identity.active_checksum | `224b3b84a278d65677d5fa9bfae7ab585007ef89079e62084a3e4cffb676e3f9` |
| identity.package_checksum | `3a75f4ad6d1f9f2a054615ac608bc6d5efd53bc4778f37e4f4bb01467fd3a37d` |
| identity.image_checksum | `224b3b84a278d65677d5fa9bfae7ab585007ef89079e62084a3e4cffb676e3f9` |
| ble[2].case_count | `22` |
| ble[2].case_results | `PASS=22` |
| ble[2].result | `PASS` |
| ble[2].rc | `0` |
| ble[2].uart_baud_rate | `460800` |
| ble[2].ble_endpoint | `5:818f070641f0` |
| ble[2].repeat | `1` |
| ble[2].monitor_duration_ms | `0` |
| ble[2].app_firmware.bytes | `863724` |
| ble[2].app_firmware.sha256 | `9326ffca36704d7d2d82d39714d40c37eaec69b1f1f42eb8c52575c0dfc1e2c9` |
| ble[2].loader_firmware.bytes | `925986` |
| ble[2].loader_firmware.sha256 | `3a75f4ad6d1f9f2a054615ac608bc6d5efd53bc4778f37e4f4bb01467fd3a37d` |
| ble[2].crash_firmware.bytes | `894831` |
| ble[2].crash_firmware.sha256 | `4819ea59881baba6fe293294c85b6d1d82074d1a2501ea882987f67c6a90c658` |
| ble[2].firmware_url.bytes | `0` |
| ble[2].coredump.expected_bytes | `2096` |
| ble[2].summary.cases | `22` |
| ble[2].summary.passed | `22` |
| ble[2].summary.failed | `0` |
| ble[2].summary.elapsed_ms | `374676` |
| uart_full | `incomplete: managed install-loader status session stalls after board convergence; independent status passes` |
| scope | `Historical acceptance before the buffered-poll firmware correction; final-v2 supersedes this package set.` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.ble[1].case[install-loader], record.ble[2].case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=224b3b84a278d65677d5fa9bfae7ab585007ef89079e62084a3e4cffb676e3f9; partition_1_package_checksum=3a75f4ad6d1f9f2a054615ac608bc6d5efd53bc4778f37e4f4bb01467fd3a37d; partition_2_image_checksum=224b3b84a278d65677d5fa9bfae7ab585007ef89079e62084a3e4cffb676e3f9; partition_2_package_checksum=3a75f4ad6d1f9f2a054615ac608bc6d5efd53bc4778f37e4f4bb01467fd3a37d` |
