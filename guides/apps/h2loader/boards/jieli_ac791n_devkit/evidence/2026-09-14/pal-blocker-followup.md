# PAL blocker follow-up — 2026-09-14

## BLE stack acceptance

Maintainer authorization now permits lifecycle test Apps through the Loader BLE path into P2. Both full suites ran through Terminal.app, using the frozen packages listed in [artifact identities](pal-review-followup-hardware.md#retained-acceptance-facts-pal-policy-fix-artifacts). UART was captured independently without writes. No USB download or format operation was used.

- [BLE run 1](pal-blocker-followup.md#retained-acceptance-facts-pal-blocker-ble-1): 22/22, 362.533 seconds.
- [BLE run 2](pal-blocker-followup.md#retained-acceptance-facts-pal-blocker-ble-2): 22/22, 361.368 seconds.

These validate the `8c235666` stack policy correction on the previous batch's final packages, before further firmware changes. They supplement the three consecutive App BLE status requests after UART installation and UART 25/25 recorded in [the preceding run](./pal-review-followup-hardware.md). They are not acceptance for subsequent source changes.

Only the JieLi App production caller uses `run_return_console`; ESP and BK do not call that synchronous entry. The Loader's shared service still creates its separate command task with the 48 KiB minimum. The four JieLi App task policies cover display, audio-system, touch and button; crash-before-confirm inherits display. The compiled session fixture checks the configured policy itself is at least 49152 bytes.

Local raw files: `tmp/jieli/pal-review-next/ble-twice.sh`, `ble-1.log`, `ble-2.log`, `ble-twice-uart.log`.

## Independent task identity defect

The PAL concurrency suite starts three consumers with one policy name and three producers with another. JieLi used that policy label as the native task name, then joined by deleting that name. The pinned SDK requires unique task names; `system.a`'s `os_api.c.o` IR implements `os_task_del` by `xTaskGetHandle(name)` followed by `vTaskDelete`. Thus a completed worker's join may delete a different live worker.

The provider now separates scheduling policy from native identity. Every live PAL task has a distinct object-derived native name; the SDK adapter resolves the original policy and supplies its priority, stack and queue settings to native creation. The requested minimum stack is respected. Generated PAL task policies use dynamic storage; static allocations cannot be shared between concurrent instances.

The real-pthread regression `test_jieli_wl82_task_identity` keeps the second same-label worker running while joining the completed first worker. Before the change, the name-based fake selects the wrong worker and the first join assertion fails. Afterward both joins finish, the second worker remains alive until released, and all allocations are reclaimed. Clang package tests, GCC `-Wall -Wextra -Werror`, and TSan pass. The SDK dispatch fixture also verifies original policy selection, stack minimum rounding, missing/static policy rejection and native errors. Native Loader, PAL, display and crash packages build successfully (91.353 seconds).

This independently demonstrated defect does not establish the cause of condition case 10. Board comparison measurements remain in progress; no Core fix claim is made here.

## Measurement protocol

Each revision uses an unmodified PAL E2E App installed through the UART Loader. PAL intentionally leaves itself unconfirmed. Consequently, after returning to Loader, repeating the same image is rejected by trial rollback protection. An initial repeated-baseline attempt stayed in Loader and an explicit App boot was rejected; neither entered Core and neither counts toward a hang rate. Subsequent measurements alternate revisions, preserving rollback protection and exact package contents. P1 identity and UART status are checked at every transition.

## O8 maintainer decision

Caller-supplied allocators are now required, matching ESP and BK, with matching allocation/free ownership and NULL/default behavior. Implementation and allocation-failure tests are pending after the Core blocker. This supersedes the earlier decision-needed entry; no compatibility mode is authorized.

## Untraced revision comparison

[Comparison summary](pal-blocker-followup.md#retained-acceptance-facts-pal-core-revision-comparison), three effective App executions per revision:

| Revision | Complete PAL pass | Core entered, no aggregate observed |
| --- | --- | --- |
| `2a814d32` | 1/3 | 2/3 |
| `9e782405` minus `c5e35a0a` | 1/3 | 2/3 |
| `9e782405` minus `57e89369` and `8c235666` | 1/3 | 2/3 |
| `9e782405` frozen final batch package | 1/3 | 2/3 |

Every complete run passed Filesystem 13, all eight Core cases (including 10 and 11), and offline Wi-Fi 27, aggregate 10/10. In each incomplete execution Filesystem 13 passed and Core suite entry was observed, with no final Core ledger. The CLI monitor exited before the script's independent observation cutoff; these records are observations of non-completion, not proof of the exact blocked instruction. Every post-run status identified the expected App image, and returning to Loader preserved the expected P1 identity. No SDK reset recovery or raw flash operation was used.

This small sample reproduces non-completion on the earlier baseline and shows that neither requested revert eliminates it. It does not identify an introducing commit or establish a repair. Local detailed logs, transition statuses and frozen reverse patches are under `tmp/jieli/pal-review-next/measurements/` and `tmp/jieli/pal-review-next/packages/`. A separate delayed diagnostic build is used next; it is not included in these rates.

The pinned `tasks.c.o` IR additionally confirms a 64-byte native task-name field and a 64-byte comparison bound in `xTaskGetHandle`, excluding 16-byte truncation of the condition task's name as an explanation.

## Core blocker outcome

Code fix: `d91c2c1f`. [Three untraced runs and delayed old-source ledger](pal-blocker-followup.md#retained-acceptance-facts-pal-core-task-identity-hardware) establish:

- The identical fixed PAL package passes three separate UART installs and untraced executions, each aggregate 10/10: Filesystem 13, Core 1/2/3/4/5/6/10/11, and offline Wi-Fi 27.
- Old `9e782405` provider sources with only a delayed ledger observer reproduce the stall with Core count 7: case 10 has returned OK, and case 11 has not returned. This avoids per-operation prints on the condition path. The earlier traced case-10 localization was not reproduced by this observation; no speculative condition-algorithm change was made.
- The deterministic pthread regression proves the failure mechanism: joining one completed worker by a nonunique SDK name can delete another live worker, leaving its later join waiting forever. The SDK name lookup examines ready/delayed/suspended task lists, not the PAL handle being joined. The provider's original integration `9c962f04` already used named labels for native creation/deletion; the observed defect predates the proposed c5/O12 reverts. This introducing-source attribution is from history inspection, not a claim that an earlier pre-integration board package was tested.

The first delayed observer registered against `app_core` produced no observation; the successful observer explicitly targets `sys_timer`. Its diagnostic patch and package are local under `tmp/jieli/pal-review-next/packages/diagnostic/old-ledger-service.patch` and `old-ledger-service.update.tar.zlib`. They are not production source or part of the three untraced passes. One incomplete diagnostic run records monitor exit code 3, about 92 seconds after Core entry, clarifying the earlier `monitor-ended` classification.

P1 remains the previously accepted Loader image `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`. Each fixed run returned through the App's UART command to Loader. Final-source Loader self-install and lifecycle acceptance will be repeated after the remaining changes.

## O8 allocator implementation

Mutex, semaphore, condition and queue objects now retain and use their caller allocator. Queue backing storage and child PAL synchronization configs use that same allocator. NULL keeps the SDK default; SDK-native handles retain their SDK create/destroy ownership. Task/timer configs have no allocator field and are unchanged. The existing App operation mutex already supplies the platform allocator; the previously omitted queue child configs are migrated.

`//tools/bazel:jieli_allocator_test` compiles the real providers with two ownership-checking allocators. All four paths fail before at the zero allocation-count assertion, under both Clang and GCC. Afterward allocation/free identity, every custom-allocation failure, native handle allocation failure, poisoned allocation contents, NULL fallback and queue data round trips pass. Existing core/condition/queue tests pass, with the threaded condition/queue suites also passing TSan. Native Loader, PAL, display and crash builds pass (70.850 seconds). [The allocator PAL package passes 10/10 on hardware](pal-blocker-followup.md#retained-acceptance-facts-pal-allocator-hardware), including all Core cases, and returns to the same valid P1 Loader.

Additional pinned IR audit: `xQueueGiveMutexRecursive` returns failure only for an owner mismatch; for the matching owner it returns success after decrementing recursion and giving the queue at zero. `os_mutex_post` additionally rejects IRQ/critical-section context. Valid PAL task-context ownership therefore has no demonstrated recoverable unlock failure requiring a new ownership policy. Mutex/semaphore deletion returns zero after `vQueueDelete`, as previously recorded. A condition's private wake semaphore starts at zero and is posted at most once before unlink, so native capacity failure is unreachable for that valid lifecycle. Mandatory relock and the internal failure/ownership report are retained; no recovery guarantee is invented for invalid native objects or external interference with their ownership.

## Affinity preservation after unique task naming

The audio-system hardware probe exposed a regression in `d91c2c1f`: the music worker starts, but the microphone worker cannot resolve its target policy and startup reports `-5`. The target's policy is `#C0audio-system/mic`; its public PAL label is `audio-system/mic`. The native decoder/microphone servers opened and were stopped during the App's startup-failure cleanup. This is not a successful audio streaming result.

Pinned `system.a:task.c.o`, `get_task_priority` (IR lines 20–61), matches both literal names and the suffix after `#C0`/`#C1`. `task_create` then passes the original prefixed policy name to `os_task_create`. In `tasks.c.o`, `prvInitialiseNewTask` (IR lines 210–249), that prefix selects CPU affinity and is removed before the TCB name is stored. The PAL adapter must preserve both steps when substituting a unique native identity: resolve the canonical label, prepend the selected affinity only for SDK creation, and retain the canonical unique name for join/deletion.

The SDK dispatch fixture now fails before for a canonical label with a CPU-0 policy. It checks CPU-0/CPU-1, explicit prefixed lookup, priority/queue/stack budgets, and unchanged anonymous/named dispatch afterward. The real-pthread identity test also fails before when a prefixed PAL label leaks into the retained deletion identity; canonical and prefixed labels both pass afterward. Clang, GCC strict-warning builds, the BLE stack-budget fixture, and TSan pass. The affinity correction is validated independently with the preceding audio provider, before the pending O1 replacement is included.

[Hardware comparison](pal-blocker-followup.md#retained-acceptance-facts-pal-task-affinity-hardware): with the affinity-only correction and the preceding audio provider, the UART-installed audio-system App reaches READY and emits 25 microphone reports during a 30-second observation. Its package SHA-256 is `36a4e1ca2cdc3c7d7154a1a8946ecfd39e19b0b68cfaeee5cd6f9eb8b761e51c`. Loader, PAL and audio-system native builds pass (64.911 seconds). The App returns through UART to the unchanged valid P1 Loader. This verifies worker creation and streaming, not the pending O1 deadline/drain replacement or a controlled stop/restart cycle.

## Retained acceptance facts: pal-allocator-hardware

Historical run summary transcribed from `pal-allocator-hardware.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| record[1].case_count | `10` |
| record[1].case_results | `0=10` |
| record[1].variant | `allocator-core` |
| record[1].iteration | `1` |
| record[1].outcome | `aggregate` |
| record[1].monitor_rc | `130` |
| record[1].elapsed_s | `33.28978312498657` |
| record[1].core_elapsed_s | `10.794066625006963` |
| record[1].package_sha256 | `3b0cb8eb54ed00ebac3d67c536d9c763044a6cd7930a01730b92b35fc50045e6` |
| record[1].image_sha256 | `5349af67c6659650718c99f0e79a67fe68cd2648195b1932e4de066a8f9bff0d` |
| record[1].core_pass | `True` |
| record[1].aggregate[1][1] | `0` |
| record[1].aggregate[1][2] | `10` |
| record[1].aggregate[1][3] | `0` |

## Retained acceptance facts: pal-blocker-ble-1

Historical run summary transcribed from `pal-blocker-ble-1.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

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
| app_firmware.bytes | `856159` |
| app_firmware.sha256 | `388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| loader_firmware.bytes | `918615` |
| loader_firmware.sha256 | `060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09` |
| crash_firmware.bytes | `887356` |
| crash_firmware.sha256 | `d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `22` |
| summary.passed | `22` |
| summary.failed | `0` |
| summary.elapsed_ms | `362533` |
| identity.active_checksum | `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`; `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |
| identity.package_checksum | `d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3`; `060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09`; `388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| identity.image_checksum | `a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e`; `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`; `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; partition_2_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3; stage_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; stage_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; partition_2_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3; stage_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; stage_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[stage-abort-after-send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; partition_2_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| record.case[install-app], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e; stage_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; stage_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_2_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09` |

## Retained acceptance facts: pal-blocker-ble-2

Historical run summary transcribed from `pal-blocker-ble-2.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

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
| app_firmware.bytes | `856159` |
| app_firmware.sha256 | `388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| loader_firmware.bytes | `918615` |
| loader_firmware.sha256 | `060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09` |
| crash_firmware.bytes | `887356` |
| crash_firmware.sha256 | `d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| firmware_url.bytes | `0` |
| coredump.expected_bytes | `2096` |
| summary.cases | `22` |
| summary.passed | `22` |
| summary.failed | `0` |
| summary.elapsed_ms | `361368` |
| identity.active_checksum | `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`; `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |
| identity.package_checksum | `d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3`; `060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09`; `388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| identity.image_checksum | `a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e`; `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`; `aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.case[help], record.case[status], record.case[stats], record.case[legacy-commands-absent], record.case[install-crash-app], record.case[coredump-status], record.case[coredump-dump], record.case[coredump-erase], record.case[coredump-status-after-erase] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; partition_2_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3; stage_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; stage_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| record.case[send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; partition_2_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3; stage_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; stage_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[stage-abort-after-send] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=a3226f1c71d01d90e0588456ddbe688f67470249694fd00bec546eec5976210e; partition_2_package_checksum=d715e9f45ff5658c95c2ccd4114d43280b5e7bd140adc8c9634b14e6a85fbec3` |
| record.case[install-app], record.case[app-help], record.case[app-status], record.case[app-stats], record.case[app-memory], record.case[app-legacy-commands-absent], record.case[app-stage-abort-after-send], record.case[reboot-app-preserves-stage] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[app-send] | `running_partition=2; next_partition=2; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e; stage_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; stage_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[reboot-loader-preserves-stage] | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=aa6e4570116018b33fc0fbd789902a422a81328b3e30b4713597ad1786e57b05; partition_2_package_checksum=388d7930bd8264219e0586b7796993761c9b17f1f8bf5cf4febb211e097e9e0e` |
| record.case[install-loader] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_1_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09; partition_2_image_checksum=6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0; partition_2_package_checksum=060d05521af7604ba3f342ebdbef779c4349df5205a44e05ac66aec945be4b09` |

## Retained acceptance facts: pal-core-revision-comparison

Historical run summary transcribed from `pal-core-revision-comparison.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| record[1].case_count | `10` |
| record[1].case_results | `0=10` |
| record[1].variant | `base-2a814d32` |
| record[1].iteration | `1` |
| record[1].outcome | `aggregate` |
| record[1].package_sha256 | `fe35c7b7869a355d97f5d7d144f9365069298775d62be77a8dc6da2f2d669dd1` |
| record[1].image_sha256 | `e7f7c043f934f2eaf9e4a875dff792aa9ed718a4845834b8ef76a28ebdaaff25` |
| record[1].core_pass | `True` |
| record[1].aggregate[1][1] | `0` |
| record[1].aggregate[1][2] | `10` |
| record[1].aggregate[1][3] | `0` |
| record[2].case_count | `1` |
| record[2].case_results | `0=1` |
| record[2].variant | `without-c5e35a0a` |
| record[2].iteration | `1` |
| record[2].outcome | `monitor-ended` |
| record[2].package_sha256 | `6f974c92a3a0f4176e66627360aa1d52ca26dfcf6f9f0a4e55a8df6051aeee30` |
| record[2].image_sha256 | `c082a1d5a7a607958512f97e7d4abd1dcb96eb1744930e460758f9ae7209ff4e` |
| record[2].core_pass | `False` |
| record[3].case_count | `1` |
| record[3].case_results | `0=1` |
| record[3].variant | `without-o12` |
| record[3].iteration | `1` |
| record[3].outcome | `monitor-ended` |
| record[3].package_sha256 | `ce8c6508bb2c6c06b1a1d0cd46083ff22ea5693be3edd4dbf71c57a5694e572c` |
| record[3].image_sha256 | `49e89fc7f6c26a4a270934b08b2c8136ea5ae982e62569dde8718c69d647c53c` |
| record[3].core_pass | `False` |
| record[4].case_count | `1` |
| record[4].case_results | `0=1` |
| record[4].variant | `current-9e782405` |
| record[4].iteration | `1` |
| record[4].outcome | `monitor-ended` |
| record[4].package_sha256 | `9f02c0c56c9f9af3ecd0a436bd16c7c71ca147cd05580a0cf87e81505b58a2af` |
| record[4].image_sha256 | `bf635cc1a18dd6e0415e0f88fd0ce34002867261b0bf3e50f4788e46436d3501` |
| record[4].core_pass | `False` |
| record[5].case_count | `1` |
| record[5].case_results | `0=1` |
| record[5].variant | `base-2a814d32` |
| record[5].iteration | `2` |
| record[5].outcome | `monitor-ended` |
| record[5].package_sha256 | `fe35c7b7869a355d97f5d7d144f9365069298775d62be77a8dc6da2f2d669dd1` |
| record[5].image_sha256 | `e7f7c043f934f2eaf9e4a875dff792aa9ed718a4845834b8ef76a28ebdaaff25` |
| record[5].core_pass | `False` |
| record[6].case_count | `1` |
| record[6].case_results | `0=1` |
| record[6].variant | `without-c5e35a0a` |
| record[6].iteration | `2` |
| record[6].outcome | `monitor-ended` |
| record[6].package_sha256 | `6f974c92a3a0f4176e66627360aa1d52ca26dfcf6f9f0a4e55a8df6051aeee30` |
| record[6].image_sha256 | `c082a1d5a7a607958512f97e7d4abd1dcb96eb1744930e460758f9ae7209ff4e` |
| record[6].core_pass | `False` |
| record[7].case_count | `1` |
| record[7].case_results | `0=1` |
| record[7].variant | `without-o12` |
| record[7].iteration | `2` |
| record[7].outcome | `monitor-ended` |
| record[7].package_sha256 | `ce8c6508bb2c6c06b1a1d0cd46083ff22ea5693be3edd4dbf71c57a5694e572c` |
| record[7].image_sha256 | `49e89fc7f6c26a4a270934b08b2c8136ea5ae982e62569dde8718c69d647c53c` |
| record[7].core_pass | `False` |
| record[8].case_count | `1` |
| record[8].case_results | `0=1` |
| record[8].variant | `current-9e782405` |
| record[8].iteration | `2` |
| record[8].outcome | `monitor-ended` |
| record[8].package_sha256 | `9f02c0c56c9f9af3ecd0a436bd16c7c71ca147cd05580a0cf87e81505b58a2af` |
| record[8].image_sha256 | `bf635cc1a18dd6e0415e0f88fd0ce34002867261b0bf3e50f4788e46436d3501` |
| record[8].core_pass | `False` |
| record[9].case_count | `1` |
| record[9].case_results | `0=1` |
| record[9].variant | `base-2a814d32` |
| record[9].iteration | `3` |
| record[9].outcome | `monitor-ended` |
| record[9].package_sha256 | `fe35c7b7869a355d97f5d7d144f9365069298775d62be77a8dc6da2f2d669dd1` |
| record[9].image_sha256 | `e7f7c043f934f2eaf9e4a875dff792aa9ed718a4845834b8ef76a28ebdaaff25` |
| record[9].core_pass | `False` |
| record[10].case_count | `10` |
| record[10].case_results | `0=10` |
| record[10].variant | `without-c5e35a0a` |
| record[10].iteration | `3` |
| record[10].outcome | `aggregate` |
| record[10].package_sha256 | `6f974c92a3a0f4176e66627360aa1d52ca26dfcf6f9f0a4e55a8df6051aeee30` |
| record[10].image_sha256 | `c082a1d5a7a607958512f97e7d4abd1dcb96eb1744930e460758f9ae7209ff4e` |
| record[10].core_pass | `True` |
| record[10].aggregate[1][1] | `0` |
| record[10].aggregate[1][2] | `10` |
| record[10].aggregate[1][3] | `0` |
| record[11].case_count | `10` |
| record[11].case_results | `0=10` |
| record[11].variant | `without-o12` |
| record[11].iteration | `3` |
| record[11].outcome | `aggregate` |
| record[11].package_sha256 | `ce8c6508bb2c6c06b1a1d0cd46083ff22ea5693be3edd4dbf71c57a5694e572c` |
| record[11].image_sha256 | `49e89fc7f6c26a4a270934b08b2c8136ea5ae982e62569dde8718c69d647c53c` |
| record[11].core_pass | `True` |
| record[11].aggregate[1][1] | `0` |
| record[11].aggregate[1][2] | `10` |
| record[11].aggregate[1][3] | `0` |
| record[12].case_count | `10` |
| record[12].case_results | `0=10` |
| record[12].variant | `current-9e782405` |
| record[12].iteration | `3` |
| record[12].outcome | `aggregate` |
| record[12].package_sha256 | `9f02c0c56c9f9af3ecd0a436bd16c7c71ca147cd05580a0cf87e81505b58a2af` |
| record[12].image_sha256 | `bf635cc1a18dd6e0415e0f88fd0ce34002867261b0bf3e50f4788e46436d3501` |
| record[12].core_pass | `True` |
| record[12].aggregate[1][1] | `0` |
| record[12].aggregate[1][2] | `10` |
| record[12].aggregate[1][3] | `0` |

## Retained acceptance facts: pal-core-task-identity-hardware

Historical run summary transcribed from `pal-core-task-identity-hardware.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| fixed_source | `d91c2c1f` |
| untraced_runs[1].case_count | `10` |
| untraced_runs[1].case_results | `0=10` |
| untraced_runs[1].variant | `identity-untraced-1` |
| untraced_runs[1].iteration | `1` |
| untraced_runs[1].outcome | `aggregate` |
| untraced_runs[1].monitor_rc | `130` |
| untraced_runs[1].elapsed_s | `38.93699358298909` |
| untraced_runs[1].core_elapsed_s | `16.389438500016695` |
| untraced_runs[1].package_sha256 | `848761c63172c7a4318990824b74093cc7d3463822466924712661e7d614f0a2` |
| untraced_runs[1].image_sha256 | `0688a23d72fdd48f36ed6477c629f041ed53a0899213fc71818df861b29fe79d` |
| untraced_runs[1].core_pass | `True` |
| untraced_runs[1].aggregate[1][1] | `0` |
| untraced_runs[1].aggregate[1][2] | `10` |
| untraced_runs[1].aggregate[1][3] | `0` |
| untraced_runs[2].case_count | `10` |
| untraced_runs[2].case_results | `0=10` |
| untraced_runs[2].variant | `identity-untraced-2` |
| untraced_runs[2].iteration | `1` |
| untraced_runs[2].outcome | `aggregate` |
| untraced_runs[2].monitor_rc | `130` |
| untraced_runs[2].elapsed_s | `33.22799970800406` |
| untraced_runs[2].core_elapsed_s | `10.476028334000148` |
| untraced_runs[2].package_sha256 | `848761c63172c7a4318990824b74093cc7d3463822466924712661e7d614f0a2` |
| untraced_runs[2].image_sha256 | `0688a23d72fdd48f36ed6477c629f041ed53a0899213fc71818df861b29fe79d` |
| untraced_runs[2].core_pass | `True` |
| untraced_runs[2].aggregate[1][1] | `0` |
| untraced_runs[2].aggregate[1][2] | `10` |
| untraced_runs[2].aggregate[1][3] | `0` |
| untraced_runs[3].case_count | `10` |
| untraced_runs[3].case_results | `0=10` |
| untraced_runs[3].variant | `identity-untraced-3` |
| untraced_runs[3].iteration | `1` |
| untraced_runs[3].outcome | `aggregate` |
| untraced_runs[3].monitor_rc | `130` |
| untraced_runs[3].elapsed_s | `33.415581874985946` |
| untraced_runs[3].core_elapsed_s | `10.706298417004291` |
| untraced_runs[3].package_sha256 | `848761c63172c7a4318990824b74093cc7d3463822466924712661e7d614f0a2` |
| untraced_runs[3].image_sha256 | `0688a23d72fdd48f36ed6477c629f041ed53a0899213fc71818df861b29fe79d` |
| untraced_runs[3].core_pass | `True` |
| untraced_runs[3].aggregate[1][1] | `0` |
| untraced_runs[3].aggregate[1][2] | `10` |
| untraced_runs[3].aggregate[1][3] | `0` |
| old_diagnostic[1].case_count | `1` |
| old_diagnostic[1].case_results | `0=1` |
| old_diagnostic[1].variant | `old-ledger-service-1` |
| old_diagnostic[1].iteration | `1` |
| old_diagnostic[1].outcome | `monitor-ended` |
| old_diagnostic[1].monitor_rc | `3` |
| old_diagnostic[1].elapsed_s | `115.09482104200288` |
| old_diagnostic[1].core_elapsed_s | `92.07327854100731` |
| old_diagnostic[1].package_sha256 | `bd768ff7f10561f4d19537d73682e75393123f36d77125cc92ae790efa92b86f` |
| old_diagnostic[1].image_sha256 | `d57a379a98d54718eeed6123e1f5d6c94ebb1a840c4300c2febfd88e7a65e9ee` |
| old_diagnostic[1].core_pass | `False` |
| old_delayed_ledger[1] | `H2_DIAG suite_index=0 case=13 result=0` |
| old_delayed_ledger[2] | `H2_DIAG suite_index=1 count=7` |
| old_delayed_ledger[3] | `H2_DIAG suite_index=1 case=1 result=0` |
| old_delayed_ledger[4] | `H2_DIAG suite_index=1 case=2 result=0` |
| old_delayed_ledger[5] | `H2_DIAG suite_index=1 case=3 result=0` |
| old_delayed_ledger[6] | `H2_DIAG suite_index=1 case=4 result=0` |
| old_delayed_ledger[7] | `H2_DIAG suite_index=1 case=5 result=0` |
| old_delayed_ledger[8] | `H2_DIAG suite_index=1 case=6 result=0` |
| old_delayed_ledger[9] | `H2_DIAG suite_index=1 case=10 result=0` |
| old_delayed_ledger[10] | `H2_DIAG suite_index=2 count=0` |

## Retained acceptance facts: pal-task-affinity-hardware

Historical run summary transcribed from `pal-task-affinity-hardware.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| record[1].case_count | `0` |
| record[1].variant | `audio-buffer-lifecycle` |
| record[1].iteration | `1` |
| record[1].outcome | `monitor-ended` |
| record[1].monitor_rc | `130` |
| record[1].elapsed_s | `66.15266520800651` |
| record[1].package_sha256 | `74fc34733963f602504012d4fac74b1a8fcd6b6102a63a9b90171af05785282f` |
| record[1].image_sha256 | `51f874d64c7b799d15c5daf1c0e058448e736faf03d6315670ce47e2140cf8b1` |
| record[1].audio_ready | `False` |
| record[1].mic_reports | `0` |
| record[2].case_count | `0` |
| record[2].variant | `audio-affinity-only` |
| record[2].iteration | `1` |
| record[2].outcome | `audio-ready-observed-30s` |
| record[2].monitor_rc | `130` |
| record[2].elapsed_s | `58.59208241599845` |
| record[2].core_elapsed_s | `30.844931083003758` |
| record[2].package_sha256 | `36a4e1ca2cdc3c7d7154a1a8946ecfd39e19b0b68cfaeee5cd6f9eb8b761e51c` |
| record[2].image_sha256 | `963aee5c844887869d26539aa515be760f4e9f4bee09fa5e6ce41b29c47ef5ef` |
| record[2].audio_ready | `True` |
| record[2].mic_reports | `25` |
