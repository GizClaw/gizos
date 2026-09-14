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

## Cached Wi-Fi status and subscriber payloads

Wi-Fi event mutations, task-side configuration publication, connect/AP wait predicates, and STA/AP status copies now share a short gate implemented with the repo's wl82 atomic helpers. Public status reads copy owned cached state and do not borrow SDK mode strings or refresh driver/LAN storage from an arbitrary reader thread. BK's provider likewise returns its cached GOT_IP snapshot; the public PAL header does not impose a live-RSSI sampling interval. JieLi refreshes link information from its connected/DHCP event path.

The SDK and subscriber calls execute outside the gate. Event dispatch copies the complete status at the same protected transition, then lends that local snapshot only for synchronous dispatch. A newer STA transition invalidates an older in-progress native refresh, so an old DHCP callback cannot publish GOT_IP after a stop. AP client-count publication is protected, but obtaining the client's borrowed SDK MAC/RSSI storage still needs the separate SDK ownership repair.

The pinned `wifi_connect.c.o:WIFI_state_on_sta_connected_hdl` invokes connected and DHCP-success callbacks from the serialized state machine (IR calls with event IDs 9 and 18). Link queries remain outside the PAL gate. The raw LAN read in the DHCP refresh and all independent netif/DNS reads still require the TCP/IP snapshot repair; this change does not claim those are fixed.

`//tools/bazel:jieli_wifi_snapshots_test` extracts the actual event, publication and reader functions. A held subscriber proves its CONNECTED payload remains stable while another thread disconnects. Concurrent event/read loops check complete state/IP-valid pairs, and a held fake SDK refresh checks that a newer stop suppresses the stale GOT_IP event. SDK/subscriber fakes check that the gate is free when invoked.

Against `c3b867ad`, strict Clang fails all three assertions, including the inconsistent GOT_IP/IP-valid pair; GCC fails the deterministic payload and stale-refresh scenarios. With TSan enabled, the baseline also fails all three assertions; no separate TSan race-warning claim is made. After repair the three scenarios pass under strict Clang/GCC and TSan. Existing scan, connect, AP-client-capacity and operation-admission fixtures retain their cases; changed C fixtures now all compile with `-Wall -Wextra -Werror` on both compilers.

Logs: `/tmp/jieli-wifi-snapshots-expanded-before.log`, `/tmp/jieli-wifi-snapshots-expanded-gcc-before.log`, `/tmp/jieli-wifi-snapshots-expanded-tsan-before.log`, `/tmp/jieli-wifi-snapshots-after.log`, `/tmp/jieli-wifi-state-regression.log`, `/tmp/jieli-wifi-state-gcc-after.log`. New host-test registration carries Linux/macOS-only compatibility. Hardware does not yet exercise these concurrent Wi-Fi transitions.

Native Loader, PAL and display packages pass in 64.336 seconds (`/tmp/jieli-wifi-state-native.log`). The iOS simulator analysis of both requested target groups also passes (`/tmp/jieli-wifi-state-ios.log`). Final-source hardware acceptance remains pending.

## Subscriber reentry into the native state machine

`bab60e6c` fixes cached status and payload ownership. A further callback-context defect remained: a spontaneous SDK event could synchronously call a subscriber while no task-side PAL operation was active. If that subscriber called disconnect/connect/AP start, admission succeeded and the SDK request waited for the same HSM callback to finish. The provider now counts active callback dispatches under the state gate and refuses new task-side operation admission with BUSY during dispatch. Existing operations retain their admission; no gate is held while invoking SDK or subscribers, and no unsubscribe-like wait is introduced.

The actual subscriber fake in `jieli_wifi_snapshots_test` attempts operation admission from every posted event. All three scenarios fail against `bab60e6c` under strict Clang/GCC before the change, then pass with strict GCC and macOS TSan. The existing real-pthread operation-admission fixture also passes with TSan; scan/connect/client regressions pass. Native Loader and PAL packages build in 33.835 seconds. Logs: `/tmp/jieli-wifi-callback-before.log`, `/tmp/jieli-wifi-callback-gcc-before.log`, `/tmp/jieli-wifi-callback-after.log`, `/tmp/jieli-wifi-callback-gcc-after.log`, `/tmp/jieli-wifi-callback-regression.log`, `/tmp/jieli-wifi-callback-native.log`. Hardware callback reentry has not been exercised.

## TCP/IP snapshots and radio lifetime

Subscriber reentry is fixed in `e91782b2`. The independent netif path now reserves task-side native radio use, copies cached PAL mode/link state, dispatches the complete IPv4/netmask/gateway/DNS copy to the TCP/IP thread, and releases the reservation before invoking a list subscriber. Radio mutations refuse admission while a snapshot owns that reservation. A changed Wi-Fi event generation rejects the result with BUSY rather than combining a prior link state with a later lease. Dispatch failure releases the reservation and returns IO. Offline reads do not enter the SDK MAC or TCP/IP paths.

The DHCP SDK callback uses the same IP-copy function, with its existing native HSM serialization protecting radio lifetime. A failed or empty IP snapshot publishes FAILED/LOST_IP rather than claiming GOT_IP with invalid data. The copying callback completes before its stack context goes out of scope. Calls already on `tcpip_thread` copy inline to avoid waiting on themselves.

Pinned SDK evidence (`/tmp/jieli-pal-review-sdk/wifi/`):

- `lwip_2_2_0.a:tcpip.c.o:tcpip_callback_wait` creates a completion semaphore, queues a callback message, waits for execution, then releases the semaphore. Its implementation does not detect self-thread calls. The worker is named `tcpip_thread`.
- `LwIP.c.o:lwip_get_netif_info` copies the current netif IP, gateway and mask. `lwip_renew` and `lwip_dhcp_release_and_stop` enqueue IP/DHCP mutation work on TCP/IP. `lwip_netif_set_up` directly mutates flags from HSM, so the PAL snapshot uses its own event-protected flags instead of reading native flags.
- `wifi_connect.c.o:WIFI_state_on_hdl` calls `Unint_LwIP` when leaving the on state. `Unint_LwIP` removes the netif and stops TCP/IP outside the TCP/IP thread. Therefore queue dispatch alone is insufficient: public snapshots additionally exclude PAL radio operations. The DHCP callback runs inside HSM serialization, before any later HSM shutdown can proceed. `lwip_event_cb` submits integer events asynchronously and does not wait for HSM completion on the TCP/IP worker.

`jieli_netif_snapshot_test` extracts the real netif provider and DHCP refresh. Its separate pthread implements the TCP/IP owner and rejects LAN/DNS reads on other threads. Baseline independent netif and DHCP refresh both fail the owner assertion with strict Clang and GCC. A separate event failure injection fails before the GOT_IP publication fix. After repair, both fixtures pass strict Clang/GCC and TSan. Cases cover IP/DNS values, same-thread capture, dispatch failure, generation rejection, offline reads, zero-capacity DNS output, and subscriber error propagation after releasing native ownership. The existing operation fixture checks that a netif reservation rejects disconnect and is released on generation mismatch. Scan/connect/client regressions pass.

Logs: `/tmp/jieli-netif-before.log`, `/tmp/jieli-netif-gcc-before.log`, `/tmp/jieli-dhcp-snapshot-before.log`, `/tmp/jieli-dhcp-snapshot-gcc-before.log`, `/tmp/jieli-dhcp-failure-before.log`, `/tmp/jieli-dhcp-failure-gcc-before.log`, `/tmp/jieli-netif-after.log`, `/tmp/jieli-netif-gcc-after.log`, `/tmp/jieli-netif-regression.log`. The new host-only target passes iOS simulator analysis (`/tmp/jieli-netif-ios.log`). Live connected Wi-Fi/DNS mutation is not yet hardware-covered.

Native Loader and PAL packages pass in 37.082 seconds (`/tmp/jieli-netif-native.log`). Final-source hardware acceptance remains pending.
