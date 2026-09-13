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
