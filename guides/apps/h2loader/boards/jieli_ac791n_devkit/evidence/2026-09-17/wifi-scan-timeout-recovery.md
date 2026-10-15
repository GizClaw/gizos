# Wi-Fi scan timeout recovery — 2026-09-17

Closes the O3 item of the [2026-09-14 PAL review](../2026-09-14/pal-review.md) and its [lifecycle page](../2026-09-14/pal-wifi-lifecycle.md). Source is PR #458 (Issue #453), accepted head `c289ae4b` on main with #456, #457 and #459; the pinned SDK is `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`. No PAL public header or other platform changed.

## SDK facts

`wifi_connect.c.o:wifi_scan_req` returns -1 without queueing when the stored mode is STA and `wifi_sta_connect_state` is `DISCONNECT`, `TIMEOUT_NOT_FOUND_SSID` or `ASSOCIAT_FAIL`, otherwise it posts HSM message 19 as a pointer message that waits for the HSM under `network_hsm_mtx`.
Only `WIFI_state_on_sta_scan_hdl` handles 19 by calling `mlme.c.o:wifi_scan` and handles 13 by invoking the PAL callback with `WIFI_EVENT_STA_SCAN_COMPLETED`; `WIFI_state_on_hdl` answers 19 by transitioning into the STA state and dropping the request, and `WIFI_state_on_ap_hdl` scans through `wifi_ap_req_scan_ssid` while neither it nor its parent handles 13.
A scan therefore completes only from an associated STA: `sync.c.o:MlmeScanReqAction` and `cmm_sync.c.o:ScanNextChannel` arm `ScanTimer` per channel (110/140 ms, 400 ms for AP), `rtmp_timer_ScanTimeout` → `ScanTimeoutAction` advances, and the last channel enqueues the confirm that reaches `wireless_main.c.o:wifi_module_scan_completed_notify` → `wifi_module_event(8)` → an integer message-13 task created by `thread_rpc`.
`wifi_off` → `network_stop` posts the off message; the On-state exit runs `wifi_module_remove` (`wait_lwip_stop_send`, `wl_rx_remove`, `tasklet_uninitialize`, `rtusb_exit`) and `Unint_LwIP`, so MLME and its timers are halted before `wifi_off()` returns, and a message-13 task that lost the mutex race is processed in the Off state, which ignores everything but message 17.
`wifi_get_scan_result` mallocs `count * 64` bytes (32 entries when `count` is 0), fills them through message 27 and returns the buffer for the caller to free; `wifi_get_mode_cur_info` reads the current mode without an HSM message.

## Provider changes

`sta_scan` returns `H2_PAL_ERR_INVALID_STATE` when the SDK mode is not STA, keeps `H2_PAL_ERR_BUSY` for the SDK's refusal while the station is not associated, and frees the result buffer.
After `H2_PAL_ERR_TIMEOUT` the status snapshot reports the association state instead of `SCANNING`; scan and connect stay `H2_PAL_ERR_BUSY` while the SDK still owns the abandoned scan.
Disconnect, AP stop and AP start are admitted while the scan is abandoned.
Since #459, STA disconnect leaves STA through `wifi_enter_smp_cfg_mode()`; `hsm.a:hsm.c.o:HSM_Tran` runs the STA exit (`wifi_module_exit_sta_mode` → `wifi_module_remove`, which halts MLME and the scan timers) before the SMP_CFG entry calls back with `WIFI_EVENT_SMP_CFG_START`, and SMP_CFG ignores the scan-completion message, so `wifi_event` releases an abandoned or reapable scan on that event without any synchronous SDK call.
AP stop and start still release it after a successful `wifi_off`, a failed `wifi_off` keeps it held, and a completion arriving after the release is ignored.
The PAL E2E image passes `H2_LOADER_CAPABILITY_WIFI` and carries the five SDK Wi-Fi driver task rows from #459 (`RtmpMlmeTask` 1400 words), so the UART `wifi scan/connect/disconnect` commands can drive the diagnostic.

## Host validation

`bazel test --config=macos_arm64 //tools/bazel:all` passes 103/103 with the extended `jieli_wifi_scan_test`, `jieli_wifi_operations_test` and `jieli_wifi_connect_test`; the operations fixture drives the real `sta_disconnect` with a fake `wifi_enter_smp_cfg_mode` and checks scan and connect stay BUSY until the event reset.
Against `main` `4fd6e947` the scan fixture fails at the `INVALID_STATE` pre-check and the operations fixture fails at `guarded_sta_disconnect == OK` while abandoned.
The threaded fixture passes under macOS `-fsanitize=thread` and Linux GCC in OrbStack; `bazel build --config=ios_sim_arm64 --nobuild --keep_going //tools/bazel:all` passes.

## Native build and board run

Packages were built in OrbStack `embed-zig-noble-amd64` with `bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build --config=ac791n --symlink_prefix=bazel-amd64-`; the head PAL package is `fc8cdc9ee91626a2106f7f9770a331f572d08f2e3a1ba65b317a650d3a804ee7` with image `54218f1d016b62a26c236b0be4671b97a39169989d4cd5a617eb51bf32c3d5cf`, and the host CLI was built from the same head with `--config=macos_arm64` because #459 added a status command bit that older CLIs reject.
On UID `d879349abc9f` (`/dev/cu.usbserial-20131240`, 460800) the package was installed from the main P1 Loader `ed7d71a6…` through the UART Loader into P2, and the boot capture reports `suite=64 case=13 result=0`, Core cases 1, 2, 3, 4, 5, 6, 10 and 11 all 0, `suite=32 case=27 result=0` and `H2_PAL_E2E result=0 passed=10 failed=0`.

The forced-timeout sequence against the bench AP returned these codes on the head image:

| Step | Result |
| --- | --- |
| Scan before any connect | -7 |
| Connect | connected |
| Scan with 1 ms budget | -6 |
| Immediate scan | -18 |
| Scan after 4 s | OK, 6 networks |
| Scan with 1 ms budget | -6 |
| Disconnect while that scan is abandoned | 0 |
| Scan right after and 3 s after disconnect | -7, -7 |
| Reconnect | connected |
| Scan | OK, 5 networks |
| Disconnect | 0 |

An A/B run isolated the release mechanism with six rounds of connect, a 1 ms scan (-6), an immediate disconnect and a reconnect 5 s later.
On the head image all six reconnects were connected and all six following scans returned 5 to 8 networks.
On an otherwise identical image with only the `WIFI_EVENT_SMP_CFG_START` reset removed (package `87aa32a7…`), the first reconnect and every later connect and scan returned -18 for all six rounds, so without the event the abandoned scan is never released and the late-completion reap cannot recover it once config mode drops the completion.
The full sequence also ran 30 more times on the head code with 5 to 300 s of idle before a further connect (90 connects, 20 of those rounds on an image adding only a stall recorder), with every connect, disconnect and final status succeeding.
After the run `reboot loader` and `stage abort` left independent status at `active_role=loader`, `running_partition=1`, `active_checksum=ed7d71a6…`, `stage_valid=0`, `last_result=0`.


## Boundaries

A completion that never arrives from an associated STA cannot be forced on hardware; the host fixture covers it, while the board shows the pre-connect refusal, the late-completion reap and the disconnect release.
One earlier pre-merge image of the same code (#458 with #459 head `ef615959`) hung once: an App `wifi connect` 2.5 minutes after a completed sequence never returned, `status` stopped answering and no watchdog reset or coredump followed, until a manual reset.
The SDK explains the silence but not the cause: every message-based SDK Wi-Fi request (`wifi_on`, `wifi_enter_sta_mode`, `wifi_enter_smp_cfg_mode`, `wifi_set_default_mode`) waits in `put_msg_to_network_connect_thread` without a timeout, so a stalled network state machine blocks the App command task that also serves `status`, while other tasks keep feeding the watchdog; the stall itself did not recur in the 30 runs above and remains open.
The bench AP password appears only on the host command line.
