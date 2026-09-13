# BLE lifetime follow-up — 2026-09-14

This is incremental O2 evidence. Final-source Loader/App BLE lifecycle acceptance remains pending; the earlier policy-fix lifecycle runs do not validate this change.

## GATT unregister

The public BLE header lends schema/callback context until successful unregister or host stop completes. ESP and BK serialize GATT access and unregister with their GATT mutex. JieLi previously copied an unprotected pointer to the characteristic and could return from unregister while its user callback still ran.

JieLi now publishes bindings and retains stack-owned callback references under a short atomic gate. It invokes user code outside the gate. External unregister closes admission and waits for retained callbacks; registration cannot replace a binding during that retirement. Calling unregister from its own callback returns `H2_PAL_ERR_BUSY`, leaving the binding intact: waiting would deadlock, while successful return would falsely release the context still borrowed by that callback. The JieLi BLEIKCP and Wi-Fi-config consumers unregister during external service teardown, after joining their workers; neither requires GATT self-unregister success. This is separate from system-event unsubscribe (O4).

`//tools/bazel:jieli_ble_gatt_lifecycle_test` extracts the actual registration, unregister and ATT-write functions. A real pthread holds a user callback while another unregisters. Before repair, both Clang and GCC fail `!atomic_load(&unregister_returned)`; the self-unregister case also fails because it incorrectly returns success. After repair both cases pass with strict `-Wall -Wextra -Werror`, Linux GCC, and macOS TSan. The fixture also checks that retirement rejects a replacement registration, retired bindings no longer dispatch, and registration works again after retirement.

Logs: `/tmp/jieli-ble-gatt-before.log`, `/tmp/jieli-ble-gatt-gcc-before.log`, `/tmp/jieli-ble-gatt-after.log`, `/tmp/jieli-ble-gatt-gcc-after.log`, `/tmp/jieli-ble-gatt-tsan.log`.

The native AC791N Loader and PAL packages build successfully (37.455 seconds, `/tmp/jieli-ble-gatt-native.log`).

Host-stop quiescence, pending INIT events, advertising command storage and connection/MTU synchronization remain open. Hardware does not yet establish concurrent unregister behavior.

## Pinned SDK facts for the remaining stop work

SDK revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, `cpu/wl82/liba/btstack.a`, decoded with the pinned compiler:

- `ble_api.h` defines stack exit with one control argument. `btstack_main.c.o` reads that argument unconditionally; the current board stop supplies zero arguments.
- `ble_stack_exit` waits for the LE run loop to exit but does not destroy the native `btstack` task. `btstack_exit` additionally shuts down the controller, posts a task-queue barrier, waits, kills the task, and releases stack memory. Its internal queue/semaphore/task-delete results are unchecked; its return alone cannot prove recovery from arbitrary internal SDK faults.
- INIT is asynchronous. The native task posts `BT_STATUS_INIT_OK` after initialization; stopping during startup must retire that event before a subsequent start can use it.
- Advertising data, extended advertising descriptors and connection-parameter requests are queued as pointers. Their storage must survive command consumption. The extended descriptors currently allocated on the board caller's stack are therefore also unsafe.
- The SDK's registered BLE-thread hook runs after command consumption. A queue-empty query from an application task is insufficient: the SDK might have dequeued a pointer without consuming it. A retirement fence must execute on the consuming thread and distinguish transactions submitted concurrently.

IR is retained in the Linux VM at `/tmp/jieli-pal-review-sdk/ble/`; these are SDK audit findings, not evidence that the remaining defects are fixed.
