# O2 lifecycle hardware acceptance — 2026-09-14

Source `1ef68a02` includes O12 stack ownership, Core task identity/affinity, O8 allocator, O1 audio and O2 BLE lifetime repairs, plus the first O3 late-scan cleanup fix. The native Loader/PAL build passed in 36.862 seconds; display/crash packages passed in 34.987 seconds. Packages were frozen before testing. No tests, timeouts or retries were changed.

| Check | Result |
| --- | --- |
| Loader self-install through UART Loader | PASS; P1 image `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| Full UART lifecycle | 25/25, 554.893 seconds; [report](./pal-o2-uart.json) |
| Normal App installed through UART, then BLE status three consecutive times | PASS, all identify App image `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |
| Full BLE lifecycle run 1 | 22/22, 312.295 seconds; [report](./pal-o2-ble-1.json) |
| Full BLE lifecycle run 2 | 22/22, 311.690 seconds; [report](./pal-o2-ble-2.json) |
| Public PAL E2E | **9/10: Filesystem 13 returns -7; all eight Core cases and offline Wi-Fi 27 pass**; [report](./pal-o2-pal.json) |

This establishes the current BLE/UART lifecycle results, not final acceptance of the remaining PAL-review scope. The Filesystem 13 failure repeats the earlier O1 hardware finding and remains a blocking O9 defect. Core condition case 10 and concurrency case 11 pass without tracing. Audio controlled stop/restart, scan-timeout recovery, extended advertising, and injected SDK teardown failures are not exercised by these lifecycle suites.

## Board and transport boundaries

Re-listed `/dev/cu.*` and checked for stale readers before opening only `/dev/cu.usbserial-20131240` at 460800. UID remained `d879349abc9f`. P1 changed only through the Loader's own upgrade flow. The first local install-script attempt rejected the archive member-name assumption before invoking any board command; the corrected script uses the manifest's `app/jieli/update.ufw` member. No USB download, format, other-port access or recovery flashing occurred.

The three App status probes and both BLE lifecycle runs used Terminal.app via `osascript`, endpoint `5:818f070641f0`. BLE suites installed their test Apps into P2 through the Loader's BLE path, as explicitly approved. Terminal windows were 45501 (three App statuses) and 45502 (two full suites). A passive UART reader recorded both BLE runs and was stopped before independent UART status. The first UART reboot-monitor case took 128.945 seconds and passed under the runner's existing reconnect logic; no additional retry or timeout change was made. An independent read-only BLE Loader status during that wait also succeeded.

The combined BLE UART capture contains one supervision timeout (`reason=8`), zero `LL_REJECT`, and 58 `adv link already open` diagnostics. The latter also appear 31 and 29 times in the earlier successful baseline captures under `tmp/jieli/codex-ble/`; they are not new to this batch. The supervision timeout was followed by advertising restart and a successful new connection. Its cause is not established, and no zero-timeout claim is made. Both full suites completed successfully. No stack-overflow text appears in this capture.

After both BLE suites, independent UART status showed valid P1 and the intentionally unconfirmed crash App in P2/staging. The subsequent PAL App was installed through UART and returned to Loader after the aggregate result. Final state for this checkpoint: valid active Loader P1 `2946bbdb…c1a8a0`, PAL App P2 and retained staging `1cd6b302…34be3ca8` (`stage_valid=1`). Exact package/image identities and status probes are in [hardware metadata](./pal-o2-hardware.json).

Raw commands and logs remain under `tmp/jieli/pal-review-next/o2-lifecycle/`: `uart.sh`, `uart.log`, `ble-twice.sh`, `ble-1.log`, `ble-2.log`, `ble-uart.log`, `loader-upgrade.log`, `before-ble.status`, and `after-ble.status`. App probe files are under `diagnostic-runs/o2-final-app-status/`; PAL logs and final returned status are under `diagnostic-runs/o2-final-pal/1/`. These are raw filenames, not VitePress links.
