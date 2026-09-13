# PAL Core / Wi-Fi real-device regression — incomplete

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
   physical reset and crash-record retrieval are pending.

## Source-confirmed coverage / implementation gaps

- SDK `include_lib/system/timer.h` documents that `sys_timer_add` and
  `sys_timeout_add` dispatch to the registering task. The Timer PAL currently
  uses those calls and requires ownership by that task. Portable PAL tasks do
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
