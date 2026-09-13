# PAL blocker follow-up — 2026-09-14

## BLE stack acceptance

Maintainer authorization now permits lifecycle test Apps through the Loader BLE path into P2. Both full suites ran through Terminal.app, using the frozen packages listed in [artifact identities](./pal-policy-fix-artifacts.json). UART was captured independently without writes. No USB download or format operation was used.

- [BLE run 1](./pal-blocker-ble-1.json): 22/22, 362.533 seconds.
- [BLE run 2](./pal-blocker-ble-2.json): 22/22, 361.368 seconds.

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

[Raw comparison records](./pal-core-revision-comparison.json), three effective App executions per revision:

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

Code fix: `d91c2c1f`. [Three untraced runs and delayed old-source ledger](./pal-core-task-identity-hardware.json) establish:

- The identical fixed PAL package passes three separate UART installs and untraced executions, each aggregate 10/10: Filesystem 13, Core 1/2/3/4/5/6/10/11, and offline Wi-Fi 27.
- Old `9e782405` provider sources with only a delayed ledger observer reproduce the stall with Core count 7: case 10 has returned OK, and case 11 has not returned. This avoids per-operation prints on the condition path. The earlier traced case-10 localization was not reproduced by this observation; no speculative condition-algorithm change was made.
- The deterministic pthread regression proves the failure mechanism: joining one completed worker by a nonunique SDK name can delete another live worker, leaving its later join waiting forever. The SDK name lookup examines ready/delayed/suspended task lists, not the PAL handle being joined. The provider's original integration `9c962f04` already used named labels for native creation/deletion; the observed defect predates the proposed c5/O12 reverts. This introducing-source attribution is from history inspection, not a claim that an earlier pre-integration board package was tested.

The first delayed observer registered against `app_core` produced no observation; the successful observer explicitly targets `sys_timer`. Its diagnostic patch and package are local under `tmp/jieli/pal-review-next/packages/diagnostic/old-ledger-service.patch` and `old-ledger-service.update.tar.zlib`. They are not production source or part of the three untraced passes. One incomplete diagnostic run records monitor exit code 3, about 92 seconds after Core entry, clarifying the earlier `monitor-ended` classification.

P1 remains the previously accepted Loader image `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`. Each fixed run returned through the App's UART command to Loader. Final-source Loader self-install and lifecycle acceptance will be repeated after the remaining changes.

## O8 allocator implementation

Mutex, semaphore, condition and queue objects now retain and use their caller allocator. Queue backing storage and child PAL synchronization configs use that same allocator. NULL keeps the SDK default; SDK-native handles retain their SDK create/destroy ownership. Task/timer configs have no allocator field and are unchanged. The existing App operation mutex already supplies the platform allocator; the previously omitted queue child configs are migrated.

`//tools/bazel:jieli_allocator_test` compiles the real providers with two ownership-checking allocators. All four paths fail before at the zero allocation-count assertion, under both Clang and GCC. Afterward allocation/free identity, every custom-allocation failure, native handle allocation failure, poisoned allocation contents, NULL fallback and queue data round trips pass. Existing core/condition/queue tests pass, with the threaded condition/queue suites also passing TSan. Native Loader, PAL, display and crash builds pass (70.850 seconds). [The allocator PAL package passes 10/10 on hardware](./pal-allocator-hardware.json), including all Core cases, and returns to the same valid P1 Loader.

Additional pinned IR audit: `xQueueGiveMutexRecursive` returns failure only for an owner mismatch; for the matching owner it returns success after decrementing recursion and giving the queue at zero. `os_mutex_post` additionally rejects IRQ/critical-section context. Valid PAL task-context ownership therefore has no demonstrated recoverable unlock failure requiring a new ownership policy. Mutex/semaphore deletion returns zero after `vQueueDelete`, as previously recorded. A condition's private wake semaphore starts at zero and is posted at most once before unlink, so native capacity failure is unreachable for that valid lifecycle. Mandatory relock and the internal failure/ownership report are retained; no recovery guarantee is invented for invalid native objects or external interference with their ownership.
