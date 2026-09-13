# PAL Core / offline Wi-Fi real-device regression — nine cases passed

Device: `3ce9e275d7aa`, UART `/dev/cu.usbserial-20131240`, 460800 baud.
The existing Loader image remained `e06de21c3b21eca04f4ea65d8062ea6a994fa601a885b5ea10395cbc434b90f6`.
Only App packages were sent using UART H2Loader `send` and `reboot upgrade`;
no USB erase or Loader self-update was performed for these runs.

The new launcher calls the existing public `h2_pal_e2e_run()` registry and uses
the shared board H2Loader layout. No board-local copy of the test cases exists.

## Observations

1. Package `07cc672f4d5f62dd8e7dbf73b8b62934cd540a1a77e3656d27a813ca0174b4db`
   installed with a matching image digest. UART and the retained crash record
   repeatedly showed `wifi get mac pending...`. Netif status called SDK
   `wifi_get_mac()` before checking `wifi_is_on()`. The offline query now
   returns before that blocking SDK call.
2. Package `c353c8188b8e7e0b435e3e1a975bc343006577d38de27f77f5374852fe39fba8`
   reached `H2_PAL_E2E result=-7 passed=8 failed=1`. The initial per-case ledger
   was missed during host reconnect, so the failing case is not identified by
   this capture. At 40.350 seconds the retained record reported
   `ASSERT-FAILD: err != OS_TIMEOUT task app_core no response` from SDK
   `event.c:268`, task `sys_event`. The launcher incorrectly kept its test and
   heartbeat loop in `app_main`, preventing SDK event dispatch. It now starts
   a PAL runner task and returns, and repeats the per-case ledger.
3. Package `6b9950591e6da04e7933ac6850e41cb4fda3510b2ccc92aa87c4c293f741bead`
   installed with a matching digest, but the new runner did not yield a captured
   ledger. A fresh UART status request timed out and an eight-second raw UART
   read produced zero bytes. This revision is **not hardware-accepted**; a
   physical reset and crash-record retrieval were required. After the user
   restarted the board, Loader status recovered. The retained record still
   had sequence `83fc6dcf` and the second run's `app_core` assertion, so it
   does not establish the third run's stop location.

## Source-confirmed coverage / implementation gaps

- SDK `include_lib/system/timer.h` documents that `sys_timer_add` and
  `sys_timeout_add` dispatch to the registering task. The Timer PAL used by
  the three runs above uses those calls and requires ownership by that task. Portable PAL tasks do
  not run the SDK event loop; this needs a proper dispatch/lifecycle adaptation,
  not an application-side message loop or a weakened test. This is not yet
  proven to be the specific failing case or the third run's stop location.
- The old Core unsupported-filesystem case wrongly expected any real provider
  to reject `mkdir("/unsupported")`. It was removed, preserving its numeric ID
  as reserved. Core now has eight cases.
- Wi-Fi currently adds one offline STA/Netif consistency case. Scan, connect,
  AP clients, reconnection, and Runtime Wi-Fi events are still uncovered here.
- The installed App intentionally remains unconfirmed for reset recovery.
  These runs do not establish successful App confirmation, self-update, BLE
  acceptance, or final-revision Loader lifecycle acceptance.

Host validation was freshly executed, not satisfied only from cached results:
`pal_e2e_test`, `pref_e2e_test` under `projects/e2e/apps/pal/app`, and
`pal_e2e_test`, `mqtt_loopback_test` under `projects/e2e/targets/cc_binary/pal`
passed. Native AC791N package compilation passed. Those results do not replace
the failed/incomplete hardware evidence above.

## Timer fix and fourth hardware run

`9d970f63` marshals Timer PAL operations to the SDK `sys_timer` service. Timer
callbacks, mutations and delayed reclamation share that service; callers no
longer need their own SDK message loop. Callback-originated operations execute
inline, and failed enqueue leaves the timer retryable. This addresses the
source-confirmed dispatch mismatch; it does not identify the third run's
actual stop location without a fresh device record.

The fourth package SHA-256 is
`0f4c979c5064d255cc773dd2a9e1b2969988680b7af0e2793cee6b45b395adcf`.
Native build and PAL core behavior tests passed. Following the user's restart,
UART Loader installed this package with a matching digest. The installed image
SHA-256 was `9afbf57b24d626b3acc6a44a63e325d4673dd163fe48874c62795ab81d811acc`.

The App repeatedly reported `H2_PAL_E2E result=0 passed=9 failed=0`, including
at uptime 45.490 seconds, beyond the second run's 40.350-second assertion.
The decoded capture includes successful case IDs 2–6, 10, 11 and 27; the Time
case's individual line was not captured, so its success is supported by the
suite aggregate rather than an individual captured line. A subsequent UART
`status` succeeded and identified this image as the running App in partition 2.
UART `reboot loader` then succeeded without physical intervention; a fresh
`status` reported partition 1, `active_role=loader`, `boot_intent=loader`, and
the unchanged Loader checksum listed above.

Local captures: `tmp/jieli/pal-e2e-send-v4.log`,
`tmp/jieli/pal-e2e-device-v4.log`, and
`tmp/jieli/pal-e2e-v4-return-loader.log`. This establishes the eight Core cases
and one offline Wi-Fi consistency case in this run, not full PAL coverage,
long-duration stability, BLE acceptance, or final Loader self-update acceptance.

## Worker-lifetime revision: fifth run, results not captured

`a326932c` retains Task/Queue/Condition worker resources after failed joins.
Its native AC791N build passed (38.616 seconds). Package
`21c71675f9d0407cc9452990bbd138fde1d473a992277b876f351e4f09142165`
was installed through UART; App status identified image
`139cfd1ad8ece10db86e39bafed8384a3c3d06e220ae5b8e8dc9ad22efa3269b`
in partition 2. Neither the upgrade monitor nor a fresh monitor session
captured a PAL result ledger. Therefore the fourth run's 9/9 result must not
be attributed to this revision. The test worker's progress is unverified;
this is not evidence of a whole-device hang because UART status and software
`reboot loader` both succeeded. Final status confirmed the unchanged Loader
in partition 1. No physical reset, USB erase, or Loader update was needed.

Captures: `tmp/jieli/pal-e2e-send-v5.log`,
`tmp/jieli/pal-e2e-device-v5.log`, `tmp/jieli/pal-e2e-monitor-v5.log`, and
`tmp/jieli/pal-e2e-v5-return-loader.log`.

## Sixth run: diagnostic build passed

The fifth revision plus optional PAL Log Core case begin/end records built
successfully (40.367 seconds). Package
`9414a52f9233c27477ca9a0164bc25e9c39340c63d8792802b75bbfdea4c9ffe`
installed through UART and repeatedly reported `result=0 passed=9 failed=0`.
App status and software return to the original Loader succeeded. The early
begin/end lines were not captured during reconnect; this run does not explain
the fifth run's missing ledger. No PAL behavior was changed between those
two builds. Captures are `tmp/jieli/pal-e2e-device-v6.log`,
`tmp/jieli/pal-e2e-status-v6.log`, and `tmp/jieli/pal-e2e-loader-status-v6.log`.
The subsequent MQTT retained-cleanup guard is host-tested, not included in
this device package.
