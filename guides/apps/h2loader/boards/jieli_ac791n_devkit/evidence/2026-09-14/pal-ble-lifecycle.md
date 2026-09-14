# BLE lifetime follow-up — 2026-09-14

This is incremental O2 evidence. GATT unregister is committed in `2137527f`; host shutdown is committed in `95c1eed2`. Final-source Loader/App BLE lifecycle acceptance remains pending; the earlier policy-fix lifecycle runs do not validate this change.

## GATT unregister

The public BLE header lends schema/callback context until successful unregister or host stop completes. ESP and BK serialize GATT access and unregister with their GATT mutex. JieLi previously copied an unprotected pointer to the characteristic and could return from unregister while its user callback still ran.

JieLi now publishes bindings and retains stack-owned callback references under a short atomic gate. It invokes user code outside the gate. External unregister closes admission and waits for retained callbacks; registration cannot replace a binding during that retirement. Calling unregister from its own callback returns `H2_PAL_ERR_BUSY`, leaving the binding intact: waiting would deadlock, while successful return would falsely release the context still borrowed by that callback. The JieLi BLEIKCP and Wi-Fi-config consumers unregister during external service teardown, after joining their workers; neither requires GATT self-unregister success. This is separate from system-event unsubscribe (O4).

`//tools/bazel:jieli_ble_gatt_lifecycle_test` extracts the actual registration, unregister and ATT-write functions. A real pthread holds a user callback while another unregisters. Before repair, both Clang and GCC fail `!atomic_load(&unregister_returned)`; the self-unregister case also fails because it incorrectly returns success. After repair both cases pass with strict `-Wall -Wextra -Werror`, Linux GCC, and macOS TSan. The fixture also checks that retirement rejects a replacement registration, retired bindings no longer dispatch, and registration works again after retirement.

Logs: `/tmp/jieli-ble-gatt-before.log`, `/tmp/jieli-ble-gatt-gcc-before.log`, `/tmp/jieli-ble-gatt-after.log`, `/tmp/jieli-ble-gatt-gcc-after.log`, `/tmp/jieli-ble-gatt-tsan.log`.

The native AC791N Loader and PAL packages build successfully (37.455 seconds, `/tmp/jieli-ble-gatt-native.log`).

The host-stop changes below add a separate quiescence boundary. Advertising command storage and connection/MTU synchronization remain open. Hardware does not yet establish concurrent unregister behavior.

## Pinned SDK facts for the remaining stop work

SDK revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, `cpu/wl82/liba/btstack.a`, decoded with the pinned compiler:

- `ble_api.h` defines stack exit with one control argument. `btstack_main.c.o` reads that argument unconditionally; the current board stop supplies zero arguments.
- `ble_stack_exit` waits for the LE run loop to exit but does not destroy the native `btstack` task. `btstack_exit` additionally shuts down the controller, posts a task-queue barrier, waits, kills the task, and releases stack memory. Its internal queue/semaphore/task-delete results are unchecked; its return alone cannot prove recovery from arbitrary internal SDK faults.
- INIT is asynchronous. The native task posts `BT_STATUS_INIT_OK` after initialization; stopping during startup must retire that event before a subsequent start can use it.
- Advertising data, extended advertising descriptors and connection-parameter requests are queued as pointers. Their storage must survive command consumption. The extended descriptors currently allocated on the board caller's stack are therefore also unsafe.
- The SDK's registered BLE-thread hook runs after command consumption. A queue-empty query from an application task is insufficient: the SDK might have dequeued a pointer without consuming it. A retirement fence must execute on the consuming thread and distinguish transactions submitted concurrently.

IR is retained in the Linux VM at `/tmp/jieli-pal-review-sdk/ble/`; these are SDK audit findings, not evidence that the remaining defects are fixed.

## Host stop and pending initialization

Public PAL operations and SDK ATT/packet callbacks now retain the host through their SDK calls and synchronous subscriber dispatch. Stop closes admission and waits for those calls without holding the gate. A pending INIT event retires startup without publishing STARTED when stop is pending. Unsolicited INIT after stop is ignored. A start admitted immediately before stop is still accounted for if it creates the native task while stop waits.

Shutdown checks advertising-disable and disconnect submission results, then uses `btstack_exit()` before releasing GATT bindings and host state. An accepted disconnect is remembered across a failed shutdown so retry does not submit it twice. Failures retain ownership and keep admission closed; they do not publish STOPPED. Since late SDK callbacks are rejected during shutdown, successful native teardown publishes one local-host DISCONNECTED event for the retired link before HOST_STOPPED; this preserves the notification BLEIKCP uses to wake stream owners, including after a failed-stop retry. Extended advertising disable uses immutable static storage because the SDK queues its pointer. The other advertising descriptors still require the separate command-storage repair.

Stop returns BUSY from a retained callback or the native `btstack` task, which cannot wait for itself. It also returns BUSY on `app_core` while INIT remains pending: that task must dispatch INIT before it can synchronously stop the host. These failures leave ownership intact.

The pinned `btstack_init` sets its private task-created flag before calling `task_create`. If creation fails, `btstack_exit` can wait forever on the missing task. Repeating initialization re-enters controller task creation; the audited `btctrler_task_init` neither checks for an existing controller task nor propagates its task-create result. There is no verified public rollback that clears this private flag and safely releases partially initialized controller state. The provider therefore quarantines this failure: start returns IO, later operations remain unavailable, and stop returns IO without entering that unsafe SDK cleanup. Reset is required. Recovery from this SDK partial-initialization failure remains an SDK-contract limitation; no successful cleanup or hardware fault-injection result is claimed.

`//tools/bazel:jieli_ble_host_lifecycle_test` extracts the real start/stop/INIT functions and retained notify/ATT wrappers. Thirteen lifecycle cases fail against `2137527f` under Clang and GCC: advertising/disconnect/full-exit errors, live notify and ATT borrows, self-stop, pending/late INIT, INIT-dispatcher self-wait, a start admitted before stop, partial SDK initialization failure, stop arriving during INIT role setup, and exactly-once disconnect notification. The INIT-publication case also fails against the intermediate shutdown implementation at the STARTED-publication assertion (`/tmp/jieli-ble-host-init-publication-before.log` and `/tmp/jieli-ble-host-init-publication-gcc-before.log`). After repair all pass, including TSan. SDK fakes re-enter the gate to check that it is not held across their calls or subscriber posts. The existing MAC-failure/ATT-connection/notify-bounds fixture also passes strict Clang and GCC; its SDK task-create failure case now expects quarantine rather than unsafe retry.

Logs: `/tmp/jieli-ble-host-before.log`, `/tmp/jieli-ble-host-gcc-before.log`, `/tmp/jieli-ble-host-after.log`, `/tmp/jieli-ble-host-gcc-after.log`, `/tmp/jieli-ble-host-tsan.log`, `/tmp/jieli-ble-connection-after.log`. After the final INIT-publication and disconnect-notification refinements, native Loader, PAL and display packages build successfully in 63.655 seconds (`/tmp/jieli-ble-host-native-complete.log`). Final-source hardware acceptance remains pending.

## Incremental App hardware check

This probe used the intermediate host-stop implementation before the final INIT-publication and disconnect-notification refinements; the structured result records its BLE source hash. It is not final-source acceptance. The native display App build completed in 32.411 seconds. After installation through UART Loader into P2, App BLE status passed three consecutive times through Terminal.app, each reporting App image `9b44b3e39c1261a38750c10df56da98cfe0db0dcd25cb485c64565ee9b5af3be`. Package SHA-256: `fc6204e74d988e9e8e516348e14de6e128942c3d52e4b3b61da1006b8646daab`.

UART return-to-Loader and subsequent status passed. P1 remains valid and unchanged at `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`; staging is empty. See [structured results](./pal-ble-host-hardware.json). The background UART capture exited without data, so this evidence relies on the three successful BLE CLI status records and the final UART status. It is not a direct test of host stop/restart, concurrent unregister, or SDK failure injection. Full final-source UART/BLE lifecycle and PAL E2E remain required.

Raw files are under `tmp/jieli/pal-review-next/diagnostic-runs/ble-host-app-status/`: `send.log`, `app.status`, `ble-status-1.log`, `ble-status-2.log`, `ble-status-3.log`, and `returned.status`.

The extended-disable descriptor fixture consumes the actual queued pointer after `h2_adv_set_stop` returns. Both Clang and GCC AddressSanitizer report `stack-use-after-return` before repair, and pass with the immutable static descriptor. Together with the 13 lifecycle cases, the baseline run has 14 failing cases. The intermediate implementation also fails the disconnected-event and retry assertions before that refinement (`/tmp/jieli-ble-host-disconnect-event-before.log`, `/tmp/jieli-ble-host-disconnect-event-gcc-before.log`).

## Connection, MTU and trace snapshots

Connection/MTU publication and readers now use the same short gate. Notify validates one coherent snapshot before its SDK call; disconnect, connection-parameter validation, MTU reads and PHY validation also snapshot under the gate. A disconnect event must have a complete packet, successful status and the current handle before clearing the link. MTU events must name the current link and carry a value between the ATT default of 23 and the locally configured 512. Their published payloads are local snapshots. ATT trace dump copies the ring under the gate and logs the copy after unlocking.

The pinned `btstack_event.h` reads the disconnect handle at bytes 3–4, status at byte 2, and MTU-event handle/MTU at bytes 2–3/4–5. Both packets contain six bytes for those fields. `att_server.c.o:att_emit_mtu_event` reports size 6. In contrast, `ll_events.c.o:hci_event_disconnection_complete` emits the four-byte `1H1` payload; `hci_controller.c.o:hci_send_event` queues all six bytes, but `hci_vendor.c.o:hci_event_handler` forwards payload length plus one as callback size (5). The disconnect guard follows that audited SDK convention and validates the payload length. A fixture using the real SDK length fails the initial six-byte guard (`/tmp/jieli-ble-sdk-disconnect-length-before.log`); it passes after correction. The provider configures the local ATT MTU to 512, so a larger negotiated value is invalid for this instance.

`//tools/bazel:jieli_ble_snapshots_test` extracts the actual connection publication, disconnect/MTU event branches, MTU getter and trace functions. Before repair, stale MTU/disconnect events, out-of-range MTUs, truncated packets and failed disconnect status corrupt or publish link state under Clang and GCC. A threaded reader also observes a torn connection/MTU pair under Clang. TSan reports races in `connected` and `h2_att_trace_dump`. After repair all eight cases pass, including real-pthread connection and trace loops; SDK/log/subscriber fakes re-enter the gate to verify it is not held across those calls. The existing ATT-connection/notify checks remain strict and cover balanced gate release on all exits.

Logs: `/tmp/jieli-ble-snapshots-before.log`, `/tmp/jieli-ble-snapshots-gcc-before.log`, `/tmp/jieli-ble-snapshots-tsan-before.log`, `/tmp/jieli-ble-snapshots-after.log`, `/tmp/jieli-ble-snapshots-gcc-after.log`, `/tmp/jieli-ble-snapshots-tsan-after.log`. Native Loader, PAL and display packages build successfully in 66.259 seconds (`/tmp/jieli-ble-snapshots-native.log`). Hardware validation for this snapshot change remains pending. Advertising state and queued advertising/connection-parameter storage are separate remaining O2 work.

## Additional command-storage audit

The pinned `hci_ll.c.o` copies legacy advertising bytes into scalar controller-task messages, but forwards extended parameters/data/enable pointers into a second queue through `btctrler_hci_cmd_to_task`. Consequently a btstack-command drain alone does **not** retire extended descriptor borrowing. The controller's `hci_ll_5_cmds.c.o` copies parameters/data and enable fields into owned advertising-group storage while processing those commands.

`system.a`'s `__os_taskq_pend` handles `Q_CALLBACK` messages in FIFO order before returning normal task messages. A callback queued to the controller after btstack has forwarded its commands can establish controller consumption. This is an audited possible fence, not a claim that the remaining command-storage fix is implemented. The original btstack-hook-only idea would be insufficient.

The actual SDK `local_irq_disable` in `apps/common/system/init.c` takes a global Bluetooth spinlock on multicore builds, so `ble_user_cmd_prepare` serializes its capacity check and enqueue across producers. The local-only sketch in the FreeRTOS port header is commented out. PAL gates must still be released before calling the SDK.

## Connection-parameter command borrowing

The pinned `btstack_main.c.o:__ble_thread_loop_handler` copies the four request fields into scalar L2CAP arguments while consuming the command. Unlike extended advertising, this pointer does not cross the controller queue. The provider now keeps one immutable request in host-owned storage until the registered command-thread hook proves consumption. A second request returns `WOULD_BLOCK` while that storage is borrowed. Registration/enqueue failures release the reservation; full native shutdown retires outstanding SDK work before clearing the host-owned storage.

The hook captures the generation and completed-submission predicate under the gate before querying command-queue emptiness on the consumer thread, then rechecks the generation. It cannot retire a request while its producer is still submitting. A native loop wake after submission covers the consumer visiting the hook too early. All SDK calls run outside the gate. ESP passes a local request to NimBLE's copying API; JieLi needs this additional retention because its pinned SDK queues a pointer.

`//tools/bazel:jieli_ble_request_lifetime_test` extracts the real request and retirement functions. All five scenarios fail against `2bb751b3` under strict Clang/GCC: queued borrowing, a producer held inside SDK submission, hook registration failure, request failure followed by reuse, and a nonempty command queue. After repair all pass, including the real-pthread submission interleaving under TSan. The fake consumes and checks all four original fields after a competing request, then proves reuse after retirement. Logs: `/tmp/jieli-ble-request-baseline.log`, `/tmp/jieli-ble-request-gcc-baseline.log`, `/tmp/jieli-ble-request-tsan.log`, `/tmp/jieli-ble-request-gcc-after.log`. Advertising storage still needs the separate second-queue fence.

Native Loader, PAL and display packages build in 64.269 seconds (`/tmp/jieli-ble-request-native.log`). The display App was installed through UART Loader into P2, then passed three consecutive BLE status sessions through Terminal.app (window 45492), each identifying image `02e3d31041c01e7466bb8e227b797a5f518ea4afa26f24bef64556b6c9574ef1`. Package SHA-256: `a630798350fc8084b959d389090a4dc85b53f1b15a0001b2426cd9a7eee8d513`. These sessions exercise reconnect after the audited disconnect-length change; the Loader BLE link worker requests connection parameters on each connection. No direct hardware failure injection or command-consumption trace is claimed.

UART return-to-Loader and status passed; P1 remains unchanged at `6b47889e361e9ae591c1e3e1a429f2d24477ab3b414847d06c3134db312627b0`, staging empty. See [structured incremental results](./pal-ble-request-hardware.json), including the exact BLE source hash. Full final-source Loader installation, PAL, UART and twice-run BLE lifecycle acceptance remains pending.
