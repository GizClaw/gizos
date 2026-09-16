# O2 lifecycle hardware acceptance — 2026-09-14

Source `1ef68a02` includes O12 stack ownership, Core task identity/affinity, O8 allocator, O1 audio and O2 BLE lifetime repairs, plus the first O3 late-scan cleanup fix. The native Loader/PAL build passed in 36.862 seconds; display/crash packages passed in 34.987 seconds. Packages were frozen before testing. No tests, timeouts or retries were changed.

| Check | Result |
| --- | --- |
| Loader self-install through UART Loader | PASS; P1 image `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| Full UART lifecycle | 25/25, 554.893 seconds; [report](pal-o2-lifecycle-hardware.md#retained-acceptance-facts-pal-o2-uart) |
| Normal App installed through UART, then BLE status three consecutive times | PASS, all identify App image `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |
| Full BLE lifecycle run 1 | 22/22, 312.295 seconds; [report](pal-o2-lifecycle-hardware.md#retained-acceptance-facts-pal-o2-ble-1) |
| Full BLE lifecycle run 2 | 22/22, 311.690 seconds; [report](pal-o2-lifecycle-hardware.md#retained-acceptance-facts-pal-o2-ble-2) |
| Public PAL E2E | **9/10: Filesystem 13 returns -7; all eight Core cases and offline Wi-Fi 27 pass**; [report](pal-o2-lifecycle-hardware.md#retained-acceptance-facts-pal-o2-pal) |

This establishes the current BLE/UART lifecycle results, not final acceptance of the remaining PAL-review scope. The Filesystem 13 failure repeats the earlier O1 hardware finding and remains a blocking O9 defect. Core condition case 10 and concurrency case 11 pass without tracing. Audio controlled stop/restart, scan-timeout recovery, extended advertising, and injected SDK teardown failures are not exercised by these lifecycle suites.

## Board and transport boundaries

Re-listed `/dev/cu.*` and checked for stale readers before opening only `/dev/cu.usbserial-20131240` at 460800. UID remained `d879349abc9f`. P1 changed only through the Loader's own upgrade flow. The first local install-script attempt rejected the archive member-name assumption before invoking any board command; the corrected script uses the manifest's `app/jieli/update.ufw` member. No USB download, format, other-port access or recovery flashing occurred.

The three App status probes and both BLE lifecycle runs used Terminal.app via `osascript`, endpoint `5:818f070641f0`. BLE suites installed their test Apps into P2 through the Loader's BLE path, as explicitly approved. Terminal windows were 45501 (three App statuses) and 45502 (two full suites). A passive UART reader recorded both BLE runs and was stopped before independent UART status. The first UART reboot-monitor case took 128.945 seconds and passed under the runner's existing reconnect logic; no additional retry or timeout change was made. An independent read-only BLE Loader status during that wait also succeeded.

The combined BLE UART capture contains one supervision timeout (`reason=8`), zero `LL_REJECT`, and 58 `adv link already open` diagnostics. The latter also appear 31 and 29 times in the earlier successful baseline captures under `tmp/jieli/codex-ble/`; they are not new to this batch. The supervision timeout was followed by advertising restart and a successful new connection. Its cause is not established, and no zero-timeout claim is made. Both full suites completed successfully. No stack-overflow text appears in this capture.

After both BLE suites, independent UART status showed valid P1 and the intentionally unconfirmed crash App in P2/staging. The subsequent PAL App was installed through UART and returned to Loader after the aggregate result. Final state for this checkpoint: valid active Loader P1 `2946bbdb…c1a8a0`, PAL App P2 and retained staging `1cd6b302…34be3ca8` (`stage_valid=1`). Exact package/image identities and status probes are in [hardware metadata](pal-o2-lifecycle-hardware.md#retained-acceptance-facts-pal-o2-hardware).

Raw commands and logs remain under `tmp/jieli/pal-review-next/o2-lifecycle/`: `uart.sh`, `uart.log`, `ble-twice.sh`, `ble-1.log`, `ble-2.log`, `ble-uart.log`, `loader-upgrade.log`, `before-ble.status`, and `after-ble.status`. App probe files are under `diagnostic-runs/o2-final-app-status/`; PAL logs and final returned status are under `diagnostic-runs/o2-final-pal/1/`. These are raw filenames, not VitePress links.

## Retained acceptance facts: pal-o2-ble-1

Historical run summary transcribed from `pal-o2-ble-1.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `22` |
| case_results | `PASS=22` |
| result | `PASS` |
| rc | `0` |
| uart_baud_rate | `460800` |
| ble_endpoint | `5:818f070641f0` |
| repeat | `1` |
| monitor_duration_ms | `0` |
| app_firmware.bytes | `860504` |
| app_firmware.sha256 | `bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| loader_firmware.bytes | `922425` |
| loader_firmware.sha256 | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| crash_firmware.bytes | `891795` |
| crash_firmware.sha256 | `029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `22` |
| summary.passed | `22` |
| summary.failed | `0` |
| summary.elapsed_ms | `312295` |
| identity.active_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0`; `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |
| identity.package_checksum | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909`; `bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d`; `029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
| identity.image_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0`; `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6`; `bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[stage-abort-after-send], record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d; stage_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; stage_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[install-app], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d; stage_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; stage_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_2_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; partition_2_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6; stage_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; stage_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |

## Retained acceptance facts: pal-o2-ble-2

Historical run summary transcribed from `pal-o2-ble-2.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `22` |
| case_results | `PASS=22` |
| result | `PASS` |
| rc | `0` |
| uart_baud_rate | `460800` |
| ble_endpoint | `5:818f070641f0` |
| repeat | `1` |
| monitor_duration_ms | `0` |
| app_firmware.bytes | `860504` |
| app_firmware.sha256 | `bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| loader_firmware.bytes | `922425` |
| loader_firmware.sha256 | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| crash_firmware.bytes | `891795` |
| crash_firmware.sha256 | `029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `22` |
| summary.passed | `22` |
| summary.failed | `0` |
| summary.elapsed_ms | `311690` |
| identity.active_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0`; `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |
| identity.package_checksum | `029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6`; `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909`; `bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| identity.image_checksum | `bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706`; `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0`; `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; partition_2_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6; stage_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; stage_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; partition_2_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6; stage_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; stage_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[stage-abort-after-send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; partition_2_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
| record.case[install-app], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d; stage_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; stage_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_2_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |

## Retained acceptance facts: pal-o2-hardware

Historical run summary transcribed from `pal-o2-hardware.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| source | `1ef68a02` |
| packages.jieli_ac791n_devkit-pal-wl82.update.tar.zlib.bytes | `885918` |
| packages.jieli_ac791n_devkit-pal-wl82.update.tar.zlib.sha256 | `aa64f778ab8fd9fbea08e33c3563264eb1b2b8cb51d98b65bf8019e6002985b9` |
| packages.jieli_ac791n_devkit-color-bar-wl82.update.tar.zlib.bytes | `860504` |
| packages.jieli_ac791n_devkit-color-bar-wl82.update.tar.zlib.sha256 | `bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| packages.jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib.bytes | `891795` |
| packages.jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib.sha256 | `029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
| packages.jieli_ac791n_devkit-loader-wl82.update.tar.zlib.bytes | `922425` |
| packages.jieli_ac791n_devkit-loader-wl82.update.tar.zlib.sha256 | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| loader_install.source | `1ef68a02` |
| loader_install.package_sha256 | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| loader_install.image_sha256 | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| loader_install.status.device_uid | `d879349abc9f` |
| loader_install.status.active_role | `loader` |
| loader_install.status.active_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| loader_install.status.active_image_size | `933705` |
| identity.partition_1_package_checksum | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| identity.partition_1_image_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| identity.partition_2_package_checksum | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909`; `bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d`; `029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6`; `aa64f778ab8fd9fbea08e33c3563264eb1b2b8cb51d98b65bf8019e6002985b9` |
| identity.partition_2_image_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0`; `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6`; `bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706`; `1cd6b3020de1d9ead04a25e6fd36de60cca6abc200095809e4033afb34be3ca8` |
| app_status.package_sha256 | `bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| app_status.image_sha256 | `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |
| app_status.p1_sha256 | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| app_status.uart_install | `pass` |
| app_status.app_status | `pass` |
| app_ble_status_three[1].device_uid | `d879349abc9f` |
| app_ble_status_three[1].active_role | `app` |
| app_ble_status_three[1].active_checksum | `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |
| app_ble_status_three[1].active_image_size | `871613` |
| app_ble_status_three[2].device_uid | `d879349abc9f` |
| app_ble_status_three[2].active_role | `app` |
| app_ble_status_three[2].active_checksum | `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |
| app_ble_status_three[2].active_image_size | `871613` |
| app_ble_status_three[3].device_uid | `d879349abc9f` |
| app_ble_status_three[3].active_role | `app` |
| app_ble_status_three[3].active_checksum | `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |
| app_ble_status_three[3].active_image_size | `871613` |
| after_ble.device_uid | `d879349abc9f` |
| after_ble.active_role | `loader` |
| after_ble.active_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| after_ble.active_image_size | `933705` |
| identity.stage_package_checksum | `029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6`; `aa64f778ab8fd9fbea08e33c3563264eb1b2b8cb51d98b65bf8019e6002985b9` |
| identity.stage_image_checksum | `bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706`; `1cd6b3020de1d9ead04a25e6fd36de60cca6abc200095809e4033afb34be3ca8` |
| combined_ble_uart.bytes | `1426384` |
| combined_ble_uart.sha256 | `3ad4646826b0bdb044939f02182704b88f684572d604f9f1a404dfc9fdc3fc5f` |
| combined_ble_uart.supervision_timeouts | `1` |
| combined_ble_uart.ll_reject | `0` |
| combined_ble_uart.adv_already_open | `58` |
| combined_ble_uart.stack_overflow_text | `0` |
| after_pal.device_uid | `d879349abc9f` |
| after_pal.active_role | `loader` |
| after_pal.active_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0` |
| after_pal.active_image_size | `933705` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.loader_install | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_2_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| record.app_ble_status_three[1], record.app_ble_status_three[2], record.app_ble_status_three[3] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.after_ble | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; partition_2_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6; stage_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; stage_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
| record.after_pal | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=1cd6b3020de1d9ead04a25e6fd36de60cca6abc200095809e4033afb34be3ca8; partition_2_package_checksum=aa64f778ab8fd9fbea08e33c3563264eb1b2b8cb51d98b65bf8019e6002985b9; stage_image_checksum=1cd6b3020de1d9ead04a25e6fd36de60cca6abc200095809e4033afb34be3ca8; stage_package_checksum=aa64f778ab8fd9fbea08e33c3563264eb1b2b8cb51d98b65bf8019e6002985b9` |

## Retained acceptance facts: pal-o2-pal

Historical run summary transcribed from `pal-o2-pal.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| record[1].case_count | `10` |
| record[1].case_results | `-7=1, 0=9` |
| record[1].variant | `o2-final-pal` |
| record[1].iteration | `1` |
| record[1].outcome | `aggregate` |
| record[1].monitor_rc | `130` |
| record[1].elapsed_s | `23.174500166991493` |
| record[1].core_elapsed_s | `0.545692500018049` |
| record[1].package_sha256 | `aa64f778ab8fd9fbea08e33c3563264eb1b2b8cb51d98b65bf8019e6002985b9` |
| record[1].image_sha256 | `1cd6b3020de1d9ead04a25e6fd36de60cca6abc200095809e4033afb34be3ca8` |
| record[1].core_pass | `True` |
| record[1].aggregate[1][1] | `-7` |
| record[1].aggregate[1][2] | `9` |
| record[1].aggregate[1][3] | `1` |

## Retained acceptance facts: pal-o2-uart

Historical run summary transcribed from `pal-o2-uart.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `25` |
| case_results | `PASS=25` |
| result | `PASS` |
| rc | `0` |
| uart_endpoint | `/dev/cu.usbserial-20131240` |
| uart_baud_rate | `460800` |
| repeat | `1` |
| monitor_duration_ms | `3000` |
| app_firmware.bytes | `860504` |
| app_firmware.sha256 | `bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| loader_firmware.bytes | `922425` |
| loader_firmware.sha256 | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| crash_firmware.bytes | `891795` |
| crash_firmware.sha256 | `029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `25` |
| summary.passed | `25` |
| summary.failed | `0` |
| summary.elapsed_ms | `554893` |
| identity.active_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0`; `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6` |
| identity.package_checksum | `d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909`; `bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d`; `029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
| identity.image_checksum | `2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0`; `b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6`; `bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[stage-abort-after-send], record.case[monitor], record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_2_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_2_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; stage_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; stage_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[reboot-loader-monitor] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_2_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909` |
| record.case[reboot-upgrade-monitor], record.case[reboot-app-monitor], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d; stage_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; stage_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=b26fef2bffaac89ed0a33a4c1dcfaa595dac0e5d301543f9f37b133f14c0f3f6; partition_2_package_checksum=bbf69e7851aef7c2a48bd6f32188f96ea3848323e2f5cf1966b66a7ba7fa1c5d` |
| record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=2946bbdb2cc9c64d7c08f430f977e0ede705dfc4806b89361469daafedc1a8a0; partition_1_package_checksum=d55308de78eeb139006dee8dd7b3e2ce529d76d48d18a7fd933836b186002909; partition_2_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; partition_2_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6; stage_image_checksum=bcd61055606326fb93cc3c36e10a957e1eed8193af4affecaeb49611a4b27706; stage_package_checksum=029d72372fed54c7394e61fd49236f36f2014705c899c805dd0ab37ebc1e31d6` |
