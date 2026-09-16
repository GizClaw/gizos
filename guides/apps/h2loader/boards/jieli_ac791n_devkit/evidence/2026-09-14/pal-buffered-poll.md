# Resume buffered serial frames — 2026-09-15

The final UART lifecycle exposed a receive-progress defect after Loader installation. The board had converged to P1 and announced READY, and the host received its matching session OPEN ACK. Status then stalled despite a responsive board. Independent UART status after terminating the stale reader passed in approximately 0.6–0.8 seconds.

A burst of OPEN retries accumulates while startup performs the upgrade. Once admitted, duplicate OPEN frames invoke a synchronous ACK callback. O5 correctly bounds physical writes by the current command deadline; an ACK callback can therefore return TIMEOUT before the frame parser finishes the burst. The parser consumes that frame but retains subsequent complete frames. Device poll loops invoked the parser only for nonempty physical reads, so an idle UART never resumed the buffered work. KCP status retransmissions periodically supplied new bytes and triggered more old OPEN ACKs, instead of promptly reaching the buffered status DATA frame.

## Evidence and correction

Host instrumentation first proved that the new conversation was admitted. KCP and POSIX serial-write traces then showed correctly encoded DATA status packets receiving session ACK control frames. A separately installed, bounded device RX diagnostic confirmed that `dev_read` supplied the correct DATA bytes: the remaining OPEN requests were in the frame parser, not stale SDK receive data. The SDK receive IR, linked `memmove`, and native filter layout were audited; no SDK or ABI change is needed.

Raw evidence is under `tmp/jieli/pal-review-next/final-acceptance/`: `app-loader-session-diagnostic.log`, `kcp-wire-diagnostic.log`, `posix-wire-diagnostic.log`, `rx-diagnostic/transition.log`, and the independent `.status` files. Diagnostic runs were deliberately stopped to check board responsiveness. They are not full lifecycle passes. The copied diagnostic runner initially lacked its dynamic-library runfiles; that host setup failure is preserved separately as `runner-dyld-error.log`. Temporary RX and host instrumentation were restored before production builds.

JieLi Loader and App, ESP, and BK now call the frame parser after an OK, TIMEOUT or WOULD_BLOCK physical read, including zero bytes. Complete buffered frames resume on the next poll. Hard physical-read errors still fail closed. The change introduces no timing adjustment, retry count, flag, additional lock, or SDK workaround. It does not change the callback's deadline or promise that a failed callback is retried.

`//tools/bazel:h2loader_buffered_poll_test` compiles each real poll owner and the real CRC/frame parser. A fake physical stream supplies twenty valid OPEN frames followed by a DATA status frame; the callback exhausts its deadline after the first OPEN. With no subsequent UART bytes, the old code leaves the DATA frame stuck. All twelve regression combinations (four owners, empty TIMEOUT/WOULD_BLOCK and hard-error-then-recovery) fail before with strict Clang and GCC. After the change they pass, as do four partial-frame cases proving that empty reads do not fabricate a completed frame. The fixture injects callback failure; it does not emulate KCP retransmission internals or SDK hardware.

Logs: `/tmp/jieli-buffered-before.log`, `/tmp/jieli-buffered-gcc-before.log`, `/tmp/jieli-buffered-after.log`, `/tmp/jieli-buffered-gcc-after.log`. Existing command-deadline, handoff and iostream tests pass. Host-only iOS analysis passes. Native Loader, Display, crash, PAL and audio-system packages build successfully in 113.697 seconds; `/tmp/jieli-buffered-native.log`. ESP/BK poll implementations have compiled host-fixture coverage here, not new native or hardware acceptance.

## Hardware acceptance

The production package set is frozen separately under `tmp/jieli/pal-review-next/final-v2/`. Loader image `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`, package `9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae`. Installation passed in 52.693 seconds, with both partitions holding the expected Loader, an empty Stage and last_result=0. The unchanged full UART suite passed all 25 cases in 331.305 seconds, including install-loader, crash rollback and coredump operations. Reboot-loader-monitor passed in 7.821 seconds. Raw results are `final-v2/install.json`, `final-v2/uart.json` and `final-v2/uart.log` under the same temporary evidence directory. PAL, BLE x2 and audio acceptance of these new packages follows separately. The earlier PAL, BLE x2 and audio passes precede this firmware correction and are retained as historical evidence in the [earlier round](./pal-final-round.md).

## Retained acceptance facts: pal-console-hardware

Historical run summary transcribed from `pal-console-hardware.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| pal[1].case_count | `10` |
| pal[1].case_results | `0=10` |
| pal[1].variant | `o5-host-raw` |
| pal[1].iteration | `1` |
| pal[1].outcome | `aggregate` |
| pal[1].monitor_rc | `130` |
| pal[1].elapsed_s | `45.1787423339847` |
| pal[1].package_sha256 | `558b2a252b6b50ee67b242c03735541e78fddeac009aae7b1e7629ae9d7b17e4` |
| pal[1].image_sha256 | `c69e69e566bc02922f883aaa4fc44c2c532a877cba0f86409ace8a899309c0f0` |
| pal[1].core_pass | `True` |
| pal[1].aggregate[1][1] | `0` |
| pal[1].aggregate[1][2] | `10` |
| pal[1].aggregate[1][3] | `0` |
| filesystem_lines[1].offset | `186715` |
| filesystem_lines[1].preceding_frame_offset | `186345` |
| filesystem_lines[1].payload_bytes | `352` |
| filesystem_lines[1].gap | `0` |
| filesystem_lines[1].crc_valid | `True` |
| filesystem_lines[2].offset | `222815` |
| filesystem_lines[2].preceding_frame_offset | `222535` |
| filesystem_lines[2].payload_bytes | `262` |
| filesystem_lines[2].gap | `0` |
| filesystem_lines[2].crc_valid | `True` |
| final_role | `loader` |
| p1_image | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| final_lifecycle_pending | `True` |
| command_budget_pal[1].case_count | `10` |
| command_budget_pal[1].case_results | `0=10` |
| command_budget_pal[1].variant | `o5-command-budget` |
| command_budget_pal[1].iteration | `1` |
| command_budget_pal[1].outcome | `aggregate` |
| command_budget_pal[1].monitor_rc | `130` |
| command_budget_pal[1].elapsed_s | `67.13585233298363` |
| command_budget_pal[1].package_sha256 | `8f8baff550fd61908be21e9e6046d3cecfa97a0f421bde9f1e1da42a946adf8a` |
| command_budget_pal[1].image_sha256 | `989d692e899728eb4ec27a19f7a19ffa9d316bba713d2c3a35e682a1d38b7038` |
| command_budget_pal[1].core_pass | `True` |
| command_budget_pal[1].aggregate[1][1] | `0` |
| command_budget_pal[1].aggregate[1][2] | `10` |
| command_budget_pal[1].aggregate[1][3] | `0` |
