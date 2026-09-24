# Host serial handoff and reboot verification

The OPEN handshake used a temporary frame parser. When a read contained its ACK and the prefix of the next CRC-valid frame, it discarded the buffered prefix at return. The new stream then delivered the remaining binary bytes to the CLI text sink, which discarded the following text line. Session setup now stops parsing exactly at the ACK and transfers every remaining byte to the new stream. This preserves complete frames, split frames and text immediately after them without changing the line filter.

One source of repeated reconnects was a different defect: the host established a session and read status promptly, but the CLI verifier rejected preserved `last_result=-7` and repeated its 120-attempt loop. Device `set_next_and_reboot()` preserves install/rollback history; it does not clear that field. App/Loader reboot verification now checks role, partitions, intent and Stage preservation without requiring installation success. Upgrade verification still requires `last_result=0`. No timeout or retry budget changed.

`//tools/bazel:h2loader_serial_handoff_test` extracts the actual handshake, CRC parser, CLI text sink and reconnect verifier. A fake serial stream places a CRC-valid DATA frame immediately after the OPEN ACK, splits at every prefix length, and checks the following PAL line with both ordinary and single-byte reads. Another case supplies a reset/READY banner and responsive Loader status with preserved install failure and requires one reconnect in under one simulated second. All three regressions fail before and pass after with strict Clang and GCC; failed-upgrade rejection remains covered. The fixture models the serial boundary and status result; it does not simulate KCP retransmission internals.

All 11 existing host/CLI and new handoff targets pass with `--config=macos_arm64`; GCC fixture and host-only iOS analysis pass. Logs: `/tmp/jieli-host-handoff-before.log`, `/tmp/jieli-host-handoff-gcc-before.log`, `/tmp/jieli-host-final-tests.log`, `/tmp/jieli-host-final-gcc.log`, `/tmp/jieli-host-final-ios.log`.

## Hardware

UID `d879349abc9f`, UART `/dev/cu.usbserial-20131240` at 460800, P1 `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17`. Traced baseline established a session at 3.762 seconds, immediately after READY at 3.759 seconds, then repeatedly disconnected after status verification. The fixed ordinary CLI completed `reboot loader` in 3.549 seconds. A separate `reboot loader --monitor` run cancelled after ten seconds and shut down in 1.139 seconds; independent UART status passed. Raw artifacts: `tmp/jieli/pal-review-next/host-transport/before-reboot-trace.log`, `fixed-reboot.log`, `fixed-monitor.log`, `fixed-independent.status`.

The ordinary fixed monitor captures all ten untraced PAL cases: Filesystem 13; Core 1–6, 10 and 11; offline Wi-Fi 27. All results and the aggregate are zero, ten passed and zero failed. [Acceptance facts](pal-host-transport.md#retained-acceptance-facts-pal-host-transport). Raw monitor, package SHA and independent App/P1 status are under `tmp/jieli/pal-review-next/diagnostic-runs/host-handoff-resumed/1/`. The earlier usage-limit interruption stopped during transfer and produced no PAL evidence; its directory is preserved separately. This proves the host fixes on the watchdog-source firmware; the final O11 firmware acceptance is separate.

## Deferred reset after an early reconnect

The final UART runner exposed an additional cause of the historical long delay: it can reconnect before the deferred reset executes, successfully read the old Loader's status, and then keep that KCP conversation across the reset. The USB-UART remains open and carries READY logs, but the MCU no longer recognizes the old conversation. The initial final-suite attempt was interrupted at this case; independent status passed immediately after killing its stale host reader. A first independent open attempted before that reader exited returned `-2` and is retained separately. No board recovery was attempted.

The shared serial transport now recognizes a standalone protocol READY banner at the start of a line after session admission and returns CLOSED. Handshake logs before the OPEN ACK bypass this check, preserving normal initial connection. The caller then opens a fresh conversation without changing any timeout or retry budget. A real-source fake serial test splits the reset banner into single bytes, requires CLOSED, then reconnects through a new READY/ACK stream. It fails before and passes after with strict Clang and GCC. All twelve host/CLI/runner targets pass; logs are `/tmp/jieli-reset-ready-before.log`, `/tmp/jieli-reset-ready-gcc-before.log`, `/tmp/jieli-reset-ready-after.log`, `/tmp/jieli-reset-ready-gcc-after.log`. The full UART suite is rerun with this shared-transport correction; its result is recorded in final acceptance.

An initial reset-detection implementation matched READY inside timestamped SDK log replay and caused a prompt false CLOSED. That failed suite attempt is preserved as `ready-prefix-failed-uart.log`. The final recognizer requires a standalone line prefix; the fixture also supplies a timestamped historical READY line and requires it not to close the session. This excludes log replay without board-specific strings or timing exceptions.

The corrected full-suite `reboot-loader-monitor` case passes in 7,842 ms, including the required 3,000 ms monitor window. This exercises reconnecting before the deferred reset and then establishing a fresh post-reset conversation. It replaces the historical approximately 129-second behavior without changing test budgets. The continuing suite's final result is recorded separately.

## Retained acceptance facts: pal-host-transport

Historical run summary transcribed from `pal-host-transport.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| pal[1].case_count | `10` |
| pal[1].case_results | `0=10` |
| pal[1].variant | `host-handoff-resumed` |
| pal[1].iteration | `1` |
| pal[1].outcome | `aggregate` |
| pal[1].monitor_rc | `130` |
| pal[1].elapsed_s | `26.771210749982856` |
| pal[1].package_sha256 | `d6b1892a11b49d4c4d61cb844e728a2a02152a9a3bc7bf1e522a2e6726d84fc7` |
| pal[1].image_sha256 | `dc33f602eb38f8355e26e0e8838c88b8f2d6ab6b00dada5835cbbf30692d034b` |
| pal[1].core_pass | `True` |
| pal[1].aggregate[1][1] | `0` |
| pal[1].aggregate[1][2] | `10` |
| pal[1].aggregate[1][3] | `0` |
| reboot_loader_seconds | `3.54896087499219` |
| monitor_cancel_seconds | `1.1385880410089158` |
