# v5 idle power-cycle recovery

The user switched power off, then on. Passive UART monitor observed serial
disconnection (`monitor waiting for serial reconnect code=-8`), reconnected,
and captured App confirmation plus new heartbeats at 7210, 12210, 17210 and
22210ms. No host reboot, send or install command was issued during this cycle.
Initial Loader startup lines were not captured after serial re-enumeration;
do not claim that this log proves every early boot step.

After stopping the monitor (exit130), an independent status command completed
successfully on UART1 at460800. UID `3ce9e275d7aa`, running partition2,
Stage empty, last_result0. App image SHA remained
`072e8c38b9b662a0b3bfb4448462c930cae810160903657e4f5dfe7dc4bbef23`;
P1 Loader image SHA remained
`fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d`.

Captured confirmation: `JIELI_APP_CONFIRM result=OK code=0 display=0 transport=0`.
Sources: `deferred-v5-powercycle-monitor.log` and
`deferred-v5-powercycle-after.status`. This proves recovery of the installed
App across this user-operated idle power cycle, not interrupted flash writes.
