# PAL review follow-up hardware — 2026-09-14

## Before the App task-policy correction

UART Loader self-upgrade used the Loader-owned flow only. No USB download, format-all, or other serial port was used. UID `d879349abc9f`, UART `/dev/cu.usbserial-20131240`, 460800 baud.

The rebuilt Loader image `20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71` passed the complete UART lifecycle: **25/25**, 556.037 seconds. [Run summary](pal-review-followup-hardware.md#retained-acceptance-facts-pal-followup-uart-lifecycle), [package identities](pal-review-followup-hardware.md#retained-acceptance-facts-pal-followup-artifacts). These results precede the subsequent task-policy correction and are not final-source acceptance.

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

The corrected color-bar App was installed through UART Loader into P2. Three independent BLE status commands, launched through Terminal.app, all returned 0 with UID `d879349abc9f` and App image `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05`. No stack overflow appears in the simultaneous UART capture. [Status results](pal-review-followup-hardware.md#retained-acceptance-facts-pal-policy-fix-ble-app-status), [exact package identities](pal-review-followup-hardware.md#retained-acceptance-facts-pal-policy-fix-artifacts). Raw evidence: `policy-ble-app-1.log` through `policy-ble-app-3.log`, their `.exit` files, and `policy-app-install.log` under the same local directory.

The full BLE lifecycle suite is **not run**: its normal cases install Apps through BLE, conflicting with this task's explicit UART-only App-install constraint. Clarification was requested and remains unanswered. No Bluetooth permission failure occurred. Three read-only App sessions are not a substitute for the full 22-case suite.

## Public PAL E2E remains blocked

[Extracted run results](pal-review-followup-hardware.md#retained-acceptance-facts-pal-followup-native-stalls).

The unchanged PAL package (`bf635cc1…` image in the identities file) passed Filesystem case 13, then stopped advancing inside Core. It produced no aggregate report and never reached offline Wi-Fi case 27. Independent UART status continued to respond. Raw evidence: `policy-pal-e2e.log`, `policy-pal-core-stall.status`.

Two diagnostic-only PAL builds mirrored existing case progress and then sync calls to UART while preserving the original providers, cases and timeouts. The first identified successful Core cases 1–6 followed by a stall in condition case 10. The second passed case 10 but later stopped advancing inside concurrency; added tracing changes scheduling and does not establish a fix. Each diagnostic App was installed only through UART Loader, and independent status remained available after each stall.

These temporary source changes were saved as `pal-sync-diagnostics.patch` and removed from the worktree. Diagnostic evidence: `pal-trace-e2e.log`, `pal-trace-after-stall.status`, `pal-sync-trace-e2e.log`, `pal-sync-trace-after-stall.status`. Neither diagnostic run counts as PAL acceptance. Root cause is not established; host pthread/TSan evidence does not close this native SDK scheduling/synchronization failure. No timeout, retry, assertion or test selection was weakened.

## Final-source UART lifecycle and board state

The policy-fix Loader was installed through its own upgrade flow, with startup event 4 returning 0. Independent UART status verified image `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`, empty Stage and `last_result=0` before the suite.

The unchanged full UART runner passed **25/25**, **551.149 seconds**, `rc=0`: [run summary](pal-review-followup-hardware.md#retained-acceptance-facts-pal-policy-fix-uart-lifecycle). It used the Loader, display and crash packages in [policy-fix identities](pal-review-followup-hardware.md#retained-acceptance-facts-pal-policy-fix-artifacts). The suite exported 2096 coredump bytes, erased them and verified zero bytes afterwards. Raw files: `policy-loader-send.log`, `policy-loader-install.log`, `policy-loader-after-install.status`, `policy-uart-lifecycle.log`, `final-board.status`.

[Final independent UART status](pal-review-followup-hardware.md#retained-acceptance-facts-pal-followup-final-board): UID `d879349abc9f`, active Loader, P1 running/next, `boot_intent=auto`, `last_result=0`, P1 image `6b47889e…`. P2 and Stage retain the crash-before-confirm App (`a3226f1c…`), rejected for boot by the successful rollback case; valid partition metadata is not proof that this deliberately crashing App is bootable. No post-suite installation changed that fixture, and no UART reader was left running. A final independent BLE status via Terminal.app also returned 0 with identical UID, image and partition fields (`final-loader-ble.status`, `final-loader-ble.exit`); it is a read-only smoke check, not the full lifecycle suite.

Firmware push acceptance remains **blocked**: public PAL Core did not complete and Wi-Fi 27 was not reached; the full BLE lifecycle is not run. UART 25/25 and BLE App status 3/3 do not override those gates.

## Retained acceptance facts: pal-followup-artifacts

Historical run summary transcribed from `pal-followup-artifacts.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| record[1].package | `jieli_ac791n_devkit-color-bar-wl82.update.tar.zlib` |
| record[1].package_bytes | `856103` |
| record[1].package_sha256 | `06941449c9ad321788873d20516e823cda0938c9292f916a109980e9a5a3ed81` |
| record[1].image_bytes | `868169` |
| record[1].image_sha256 | `e86dd350d34348a9ce1103ee74d451097b5162fcc7720f592ac6dc2643b4083b` |
| record[2].package | `jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib` |
| record[2].package_bytes | `887309` |
| record[2].package_sha256 | `0c5c046cd3def06f760c6002db7048662ef2dfb7b4eebf628a365719d3631033` |
| record[2].image_bytes | `899433` |
| record[2].image_sha256 | `e8085bf01b2b1335f995dbf2255c8cbff5627a5d59e6291a552a224c071e4141` |
| record[3].package | `jieli_ac791n_devkit-loader-wl82.update.tar.zlib` |
| record[3].package_bytes | `918572` |
| record[3].package_sha256 | `ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e` |
| record[3].image_bytes | `930237` |
| record[3].image_sha256 | `20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71` |
| record[4].package | `jieli_ac791n_devkit-pal-wl82.update.tar.zlib` |
| record[4].package_bytes | `882042` |
| record[4].package_sha256 | `9f02c0c56c9f9af3ecd0a436bd16c7c71ca147cd05580a0cf87e81505b58a2af` |
| record[4].image_bytes | `894013` |
| record[4].image_sha256 | `bf635cc1a18dd6e0415e0f88fd0ce34002867261b0bf3e50f4788e46436d3501` |

## Retained acceptance facts: pal-followup-final-board

Historical run summary transcribed from `pal-followup-final-board.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| device_uid | `d879349abc9f` |
| active_role | `loader` |
| active_checksum | `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0` |
| active_image_size | `930237` |
| identity.stage_package_checksum | `d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| identity.stage_image_checksum | `a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e` |
| identity.partition_1_package_checksum | `060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09` |
| identity.partition_1_image_checksum | `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0` |
| identity.partition_2_package_checksum | `d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| identity.partition_2_image_checksum | `a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; partition_2_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3; stage_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; stage_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |

## Retained acceptance facts: pal-followup-native-stalls

Historical run summary transcribed from `pal-followup-native-stalls.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| record[1].configuration | `unmodified PAL target` |
| record[1].aggregate_report_seen | `False` |
| record[1].suite_results[1].suite | `64` |
| record[1].suite_results[1].case | `13` |
| record[1].suite_results[1].result | `0` |
| record[1].wifi_27_reached | `False` |
| record[2].configuration | `temporary case-log mirror` |
| record[2].aggregate_report_seen | `False` |
| record[2].suite_results[1].suite | `64` |
| record[2].suite_results[1].case | `13` |
| record[2].suite_results[1].result | `0` |
| record[2].core_case_ends[1].case | `1` |
| record[2].core_case_ends[1].result | `0` |
| record[2].core_case_ends[2].case | `2` |
| record[2].core_case_ends[2].result | `0` |
| record[2].core_case_ends[3].case | `3` |
| record[2].core_case_ends[3].result | `0` |
| record[2].core_case_ends[4].case | `4` |
| record[2].core_case_ends[4].result | `0` |
| record[2].core_case_ends[5].case | `5` |
| record[2].core_case_ends[5].result | `0` |
| record[2].core_case_ends[6].case | `6` |
| record[2].core_case_ends[6].result | `0` |
| record[2].wifi_27_reached | `False` |
| record[3].configuration | `temporary case-log and sync-call mirror` |
| record[3].aggregate_report_seen | `False` |
| record[3].suite_results[1].suite | `64` |
| record[3].suite_results[1].case | `13` |
| record[3].suite_results[1].result | `0` |
| record[3].core_case_ends[1].case | `1` |
| record[3].core_case_ends[1].result | `0` |
| record[3].core_case_ends[2].case | `2` |
| record[3].core_case_ends[2].result | `0` |
| record[3].core_case_ends[3].case | `3` |
| record[3].core_case_ends[3].result | `0` |
| record[3].core_case_ends[4].case | `4` |
| record[3].core_case_ends[4].result | `0` |
| record[3].core_case_ends[5].case | `5` |
| record[3].core_case_ends[5].result | `0` |
| record[3].core_case_ends[6].case | `6` |
| record[3].core_case_ends[6].result | `0` |
| record[3].core_case_ends[7].case | `10` |
| record[3].core_case_ends[7].result | `0` |
| record[3].wifi_27_reached | `False` |

## Retained acceptance facts: pal-followup-uart-lifecycle

Historical run summary transcribed from `pal-followup-uart-lifecycle.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

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
| app_firmware.bytes | `856103` |
| app_firmware.sha256 | `06941449c9ad321788873d20516e823cda0938c9292f916a109980e9a5a3ed81` |
| loader_firmware.bytes | `918572` |
| loader_firmware.sha256 | `ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e` |
| crash_firmware.bytes | `887309` |
| crash_firmware.sha256 | `0c5c046cd3def06f760c6002db7048662ef2dfb7b4eebf628a365719d3631033` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `25` |
| summary.passed | `25` |
| summary.failed | `0` |
| summary.elapsed_ms | `556037` |
| identity.active_checksum | `20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71`; `e86dd350d34348a9ce1103ee74d451097b5162fcc7720f592ac6dc2643b4083b` |
| identity.package_checksum | `ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e`; `06941449c9ad321788873d20516e823cda0938c9292f916a109980e9a5a3ed81`; `0c5c046cd3def06f760c6002db7048662ef2dfb7b4eebf628a365719d3631033` |
| identity.image_checksum | `20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71`; `e86dd350d34348a9ce1103ee74d451097b5162fcc7720f592ac6dc2643b4083b`; `e8085bf01b2b1335f995dbf2255c8cbff5627a5d59e6291a552a224c071e4141` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[stage-abort-after-send], record.case[monitor], record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_1_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e; partition_2_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_2_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_1_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e; partition_2_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_2_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e; stage_image_checksum=e86dd350d34348a9ce1103ee74d451097b5162fcc7720f592ac6dc2643b4083b; stage_package_checksum=06941449c9ad321788873d20516e823cda0938c9292f916a109980e9a5a3ed81` |
| record.case[reboot-loader-monitor] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_1_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e; partition_2_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_2_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e` |
| record.case[reboot-upgrade-monitor], record.case[reboot-app-monitor], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_1_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e; partition_2_image_checksum=e86dd350d34348a9ce1103ee74d451097b5162fcc7720f592ac6dc2643b4083b; partition_2_package_checksum=06941449c9ad321788873d20516e823cda0938c9292f916a109980e9a5a3ed81` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_1_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e; partition_2_image_checksum=e86dd350d34348a9ce1103ee74d451097b5162fcc7720f592ac6dc2643b4083b; partition_2_package_checksum=06941449c9ad321788873d20516e823cda0938c9292f916a109980e9a5a3ed81; stage_image_checksum=e86dd350d34348a9ce1103ee74d451097b5162fcc7720f592ac6dc2643b4083b; stage_package_checksum=06941449c9ad321788873d20516e823cda0938c9292f916a109980e9a5a3ed81` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_1_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e; partition_2_image_checksum=e86dd350d34348a9ce1103ee74d451097b5162fcc7720f592ac6dc2643b4083b; partition_2_package_checksum=06941449c9ad321788873d20516e823cda0938c9292f916a109980e9a5a3ed81` |
| record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_1_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e; partition_2_image_checksum=e8085bf01b2b1335f995dbf2255c8cbff5627a5d59e6291a552a224c071e4141; partition_2_package_checksum=0c5c046cd3def06f760c6002db7048662ef2dfb7b4eebf628a365719d3631033; stage_image_checksum=e8085bf01b2b1335f995dbf2255c8cbff5627a5d59e6291a552a224c071e4141; stage_package_checksum=0c5c046cd3def06f760c6002db7048662ef2dfb7b4eebf628a365719d3631033` |

## Retained acceptance facts: pal-policy-fix-artifacts

Historical run summary transcribed from `pal-policy-fix-artifacts.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| record[1].package | `jieli_ac791n_devkit-color-bar-wl82.update.tar.zlib` |
| record[1].package_bytes | `856159` |
| record[1].package_sha256 | `388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record[1].image_bytes | `868169` |
| record[1].image_sha256 | `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |
| record[2].package | `jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib` |
| record[2].package_bytes | `887356` |
| record[2].package_sha256 | `d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| record[2].image_bytes | `899433` |
| record[2].image_sha256 | `a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e` |
| record[3].package | `jieli_ac791n_devkit-loader-wl82.update.tar.zlib` |
| record[3].package_bytes | `918615` |
| record[3].package_sha256 | `060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09` |
| record[3].image_bytes | `930237` |
| record[3].image_sha256 | `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0` |
| record[4].package | `jieli_ac791n_devkit-pal-wl82.update.tar.zlib` |
| record[4].package_bytes | `882042` |
| record[4].package_sha256 | `9f02c0c56c9f9af3ecd0a436bd16c7c71ca147cd05580a0cf87e81505b58a2af` |
| record[4].image_bytes | `894013` |
| record[4].image_sha256 | `bf635cc1a18dd6e0415e0f88fd0ce34002867261b0bf3e50f4788e46436d3501` |

## Retained acceptance facts: pal-policy-fix-ble-app-status

Historical run summary transcribed from `pal-policy-fix-ble-app-status.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| record[1].case | `1` |
| record[1].exit_code | `0` |
| record[1].status.device_uid | `d879349abc9f` |
| record[1].status.active_role | `app` |
| record[1].status.active_checksum | `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |
| record[1].status.active_image_size | `868169` |
| identity.partition_1_package_checksum | `ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e` |
| identity.partition_1_image_checksum | `20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71` |
| identity.partition_2_package_checksum | `388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| identity.partition_2_image_checksum | `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |
| record[2].case | `2` |
| record[2].exit_code | `0` |
| record[2].status.device_uid | `d879349abc9f` |
| record[2].status.active_role | `app` |
| record[2].status.active_checksum | `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |
| record[2].status.active_image_size | `868169` |
| record[3].case | `3` |
| record[3].exit_code | `0` |
| record[3].status.device_uid | `d879349abc9f` |
| record[3].status.active_role | `app` |
| record[3].status.active_checksum | `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |
| record[3].status.active_image_size | `868169` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record[1], record[2], record[3] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=20ffe2acebec1c4d11ef6d975b82affaed6150359aca288c6f568299c8b51c71; partition_1_package_checksum=ea9bb0f6ef4b71d04afc286ffcfeb7033d61edb1fef8dd4ca8008fb0e961828e; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |

## Retained acceptance facts: pal-policy-fix-uart-lifecycle

Historical run summary transcribed from `pal-policy-fix-uart-lifecycle.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

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
| app_firmware.bytes | `856159` |
| app_firmware.sha256 | `388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| loader_firmware.bytes | `918615` |
| loader_firmware.sha256 | `060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09` |
| crash_firmware.bytes | `887356` |
| crash_firmware.sha256 | `d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `25` |
| summary.passed | `25` |
| summary.failed | `0` |
| summary.elapsed_ms | `551149` |
| identity.active_checksum | `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`; `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |
| identity.package_checksum | `060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09`; `388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e`; `d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| identity.image_checksum | `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`; `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05`; `a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[stage-abort-after-send], record.case[monitor], record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_2_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_2_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; stage_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; stage_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[reboot-loader-monitor] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_2_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09` |
| record.case[reboot-upgrade-monitor], record.case[reboot-app-monitor], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e; stage_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; stage_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; partition_2_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3; stage_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; stage_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
