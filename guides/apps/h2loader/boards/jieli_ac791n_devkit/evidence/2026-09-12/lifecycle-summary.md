# Historical Loader lifecycle — 2026-09-12

These are historical checkpoints, not acceptance of the current source. Run identities, outcomes and status transitions are retained below; no raw capture is required to read them. Missing fields were not recorded and must not be inferred from another run.

## Retained acceptance facts: artifacts

Historical run summary transcribed from `artifacts.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| loader.bytes | `912728` |
| loader.sha256 | `f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f` |
| color-bar.bytes | `857725` |
| color-bar.sha256 | `757862c536f460a7c1a09bb68a67e7f1ac3b07847389fd9802f65a025e2f718d` |
| crash-before-confirm.bytes | `887467` |
| crash-before-confirm.sha256 | `a0d4aacb54c3aa03097bbb23319c0aeb7645b22ac8f50e009f406edad6ec843f` |

## Retained acceptance facts: uart-ble-lifecycle

Historical run summary transcribed from `uart-ble-lifecycle.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| case_count | `46` |
| case_results | `PASS=46` |
| result | `PASS` |
| rc | `0` |
| uart_endpoint | `/dev/cu.usbserial-21240` |
| uart_baud_rate | `460800` |
| ble_endpoint | `5:aa55a7cd7239` |
| repeat | `1` |
| monitor_duration_ms | `3000` |
| app_firmware.bytes | `857725` |
| app_firmware.sha256 | `757862c536f460a7c1a09bb68a67e7f1ac3b07847389fd9802f65a025e2f718d` |
| loader_firmware.bytes | `912728` |
| loader_firmware.sha256 | `f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f` |
| crash_firmware.bytes | `887467` |
| crash_firmware.sha256 | `a0d4aacb54c3aa03097bbb23319c0aeb7645b22ac8f50e009f406edad6ec843f` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `46` |
| summary.passed | `46` |
| summary.failed | `0` |
| summary.elapsed_ms | `746904` |
| identity.active_checksum | `d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b`; `c95aa5f6e5acfcb540060496bb44c88ed8b2a7dbc60fa879288a886c8dc28026` |
| identity.package_checksum | `f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f`; `757862c536f460a7c1a09bb68a67e7f1ac3b07847389fd9802f65a025e2f718d`; `a0d4aacb54c3aa03097bbb23319c0aeb7645b22ac8f50e009f406edad6ec843f` |
| identity.image_checksum | `d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b`; `c95aa5f6e5acfcb540060496bb44c88ed8b2a7dbc60fa879288a886c8dc28026`; `d03f6af17a796aa216afb1ea052835ae7d7f27870091d5f570d484f1c1135a07` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[stage-abort-after-send], record.case[monitor], record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_1_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f; partition_2_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_2_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_1_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f; partition_2_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_2_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f; stage_image_checksum=c95aa5f6e5acfcb540060496bb44c88ed8b2a7dbc60fa879288a886c8dc28026; stage_package_checksum=757862c536f460a7c1a09bb68a67e7f1ac3b07847389fd9802f65a025e2f718d` |
| record.case[reboot-loader-monitor] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_1_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f; partition_2_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_2_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f` |
| record.case[reboot-upgrade-monitor], record.case[reboot-app-monitor], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage], record.case[install-app] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_1_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f; partition_2_image_checksum=c95aa5f6e5acfcb540060496bb44c88ed8b2a7dbc60fa879288a886c8dc28026; partition_2_package_checksum=757862c536f460a7c1a09bb68a67e7f1ac3b07847389fd9802f65a025e2f718d` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_1_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f; partition_2_image_checksum=c95aa5f6e5acfcb540060496bb44c88ed8b2a7dbc60fa879288a886c8dc28026; partition_2_package_checksum=757862c536f460a7c1a09bb68a67e7f1ac3b07847389fd9802f65a025e2f718d; stage_image_checksum=c95aa5f6e5acfcb540060496bb44c88ed8b2a7dbc60fa879288a886c8dc28026; stage_package_checksum=757862c536f460a7c1a09bb68a67e7f1ac3b07847389fd9802f65a025e2f718d` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_1_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f; partition_2_image_checksum=c95aa5f6e5acfcb540060496bb44c88ed8b2a7dbc60fa879288a886c8dc28026; partition_2_package_checksum=757862c536f460a7c1a09bb68a67e7f1ac3b07847389fd9802f65a025e2f718d` |
| record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=d16d5db1eec54e3410ca6eafdf03836970d25064ed44efdbb201972dc059125b; partition_1_package_checksum=f3e064e6c09932abd8b04644607799f18038fc7cc48a38631300f54db57af07f; partition_2_image_checksum=d03f6af17a796aa216afb1ea052835ae7d7f27870091d5f570d484f1c1135a07; partition_2_package_checksum=a0d4aacb54c3aa03097bbb23319c0aeb7645b22ac8f50e009f406edad6ec843f; stage_image_checksum=d03f6af17a796aa216afb1ea052835ae7d7f27870091d5f570d484f1c1135a07; stage_package_checksum=a0d4aacb54c3aa03097bbb23319c0aeb7645b22ac8f50e009f406edad6ec843f` |
