# P1 partial-header diagnostic: incomplete attempt

This is **not** a passing physical power-loss result.

Device UID: `3ce9e275d7aa`; UART1 at 460800 baud. The diagnostic package
`partial-p1-header-v1.tar.zlib` was 918045 bytes, SHA-256
`76ab03c003125b6f78dd46ae721af93b2a5670123e7c638e7cef88c3e75be0ba`.
The device accepted the complete Stage and verified this digest. Its image
digest was `75c57d594d92f9ad7637b11382e8360ecdba3e8e2622d2feafb3c0376dfc515a`.

After `reboot upgrade`, the candidate ran in P2, confirmed itself, published
its recovery header and began copying into P1. The diagnostic readback
reported `H2_JIELI_PARTIAL_P1_READY prefix=16 p2_crc=valid` at 7.118 seconds.
However, at 12.110 seconds the caller's bounded burn wait timed out:

```text
H2_JIELI_UPDATE_BURN call=0 pend=11 result=-6
H2_JIELI_UPDATE_EXIT_ENTER
[00:00:12.111][Info]: [UPDATE]kill update task : dw_update
[00:00:12.112][Info]: [UPDATE]clr update boot info:4000 10
H2_JIELI_STARTUP_EVENT event=5 code=-6
```

No physical power cycle was confirmed during the partial-header interval.
The cleanup changed P1's boot header, so a later reset cannot prove recovery
from that partial header. Subsequent UART status succeeded with
`running_partition=2`, `stage_valid=1`, and `last_result=-6`. Metadata reported
both partitions valid; this is not evidence that P1's native header survived.

The revised diagnostic sets an atomic paused flag. The burn-wait caller
keeps the fixture alive after timeout only when that flag is set; production
has a weak default returning false and retains its normal timeout behavior.
Host tests exercise the actual timeout branch and the fixture's guards,
and the revised package builds. Physical retry remains pending, as does
Preference-write interruption acceptance. Neither is claimed complete here.

Raw local capture: `tmp/jieli/partial-p1-header-v1-monitor.log`.
