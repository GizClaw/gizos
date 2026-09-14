# Wi-Fi lifecycle follow-up — 2026-09-14

This is incremental O3 evidence. Snapshot synchronization and recovery when a scan never completes remain open. Final-source hardware acceptance is pending.

## Late completion and SDK callback reentrancy

Pinned SDK revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, `cpu/wl82/liba/wl_wifi.a`, decoded with the pinned compiler:

- `wifi_connect.c.o:put_msg_to_network_connect_thread` serializes pointer API messages under `network_hsm_mtx`, starts a native RPC task, and waits for completion before releasing the mutex.
- `network_connect_thread` takes that same mutex for integer SDK events. `WIFI_state_on_sta_scan_hdl` invokes scan-completed callbacks while this state-machine serialization is active.
- `wifi_clear_scan_result` submits a synchronous pointer API message (28). Calling it from scan completion therefore waits on the state machine that is still executing the callback.

The previous timeout cleanup did exactly that when `SCAN_ABANDONED` received a late completion. The callback now only publishes `SCAN_REAPABLE`. The next admitted task-side Wi-Fi operation claims cleanup, clears SDK results outside the PAL operation-admission atomic, then publishes IDLE. Concurrent task operations remain excluded until cleanup finishes. No result storage is cleared while the native scan is still pending.

`//tools/bazel:jieli_wifi_scan_test` extracts the actual phase transitions and scan implementation. Its fake SDK rejects synchronous cleanup from the callback, reproducing the defect with both strict Clang and GCC (`/tmp/jieli-wifi-cleanup-before.log`, `/tmp/jieli-wifi-cleanup-gcc-before.log`). After repair all three fixture tests pass. `//tools/bazel:jieli_wifi_operations_test` additionally extracts actual cleanup and admission code, verifies that late completion leaves cleanup pending until task admission, and exercises concurrent operation exclusion with real pthreads. It passes strict Clang/GCC and macOS TSan. Logs: `/tmp/jieli-wifi-cleanup-after.log`, `/tmp/jieli-wifi-cleanup-gcc-after.log`, `/tmp/jieli-wifi-cleanup-tsan.log`.

Native Loader and PAL packages build successfully in 36.862 seconds (`/tmp/jieli-wifi-cleanup-native.log`). This incremental change has not yet been exercised on hardware.

## Recovery audit still in progress

`wifi_off` synchronously transitions the SDK state machine off and tears down its driver and lwIP. However, integer events use separate native RPC tasks: `system.a:os_api.c.o:thread_rpc` creates a task for each request, and the Wi-Fi caller does not retain its task ID. A previously queued scan event can already be waiting on `network_hsm_mtx`. The audit has not established that radio shutdown drains these events before a new scan begins. Consequently shutdown plus a delay is not accepted as proof of scan-generation quiescence, and permanently missing completion remains open.

Decoded IR is retained in the Linux VM under `/tmp/jieli-pal-review-sdk/wifi/`. No scan-timeout hardware recovery result is claimed.
