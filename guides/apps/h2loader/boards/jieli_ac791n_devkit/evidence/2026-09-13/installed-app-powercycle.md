# Installed App restart after physical power cycle

The user reported switching board power off and back on. A continuously open
UART1 monitor captured the subsequent Loader startup and automatic App launch;
no host send, install or software reboot command was issued during observation.
This checks idle power-cycle recovery, not power interruption during flash writes.

Device UID: `3ce9e275d7aa`. Loader v2 image SHA:
`3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8`.
Installed color-bar App SHA before and after:
`072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23`.

Captured sequence from `deferred-powercycle-monitor.log`:

```text
H2_JIELI_LOADER_BOOT reset_reason=0x200 next=runtime_config
H2_JIELI_SET_NEXT requested=2 running=1 active=0 committed=0 candidate=0 bytes=0
H2_JIELI_POWER_REBOOT running=1 next=2 committed=0 reason=0
JIELI_APP_CONFIRM result=OK code=0 display=0 transport=0
JIELI_HEARTBEAT uptime_ms=7170 heap_free=7514376 heap_peak=208960
JIELI_HEARTBEAT uptime_ms=12170 heap_free=7514376 heap_peak=208960
```

After stopping that monitor, a separate successful UART status query reported
`active_role=app`, `running_partition=2`, `next_partition=2`, `boot_intent=auto`,
`stage_valid=0`, `last_result=0` and the same App SHA. The initial Loader reset
reason is recorded verbatim; the subsequent App reports SOFT because Loader
performs the software handoff. Power removal itself is user-reported, not
measured by an instrument. The newer explicit-codec/reboot-callback changes
are not covered by this v2 observation.
