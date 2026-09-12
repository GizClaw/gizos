# v5 candidate Loader pre-confirm recovery

UART1, 460800, UID `3ce9e275d7aa`. Test package SHA
`292a84f874f0e1e52393c41f546e34a2feeeb23aed8ec089f948ffbf76ab0b99`
(917032 bytes), candidate image SHA
`9ad8119f9e68ab572ac221e8db499e74288e47e8eccc8aeaf9f78117a5106cb1`.
The separate test target shares the v5 production layout, launcher and policy,
and injects a recorded assertion plus reset at boot stage105 before confirmation.

Without manual reset or DL, the board returned to native P1 version14. Status
after stopping the monitor confirmed the original Loader image SHA
`fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d`,
`running_partition=1`, candidate retained in P2/Stage, `last_result=-7`.
The log contains no candidate confirmation/header-publication event. UART
status and coredump export both completed successfully.

Exported coredump decoder result:

```text
valid=true magic=0x52433248 version=2 size=2096 sequence=2214358482 reset_reason=0x80000000 boot_stage=105 caller=0x48324c43 marker_result=0 log_bytes=2048 log_total=63703 checksum=0x1f1c6c41 expected=0x1f1c6c41 committed=0x54494d43
```

Sources: `deferred-v5-crash-install.log`, `deferred-v5-crash-after.status`,
`deferred-v5-loader-crash.coredump`. This verifies deterministic pre-confirm
failure recovery, not arbitrary exceptions or interrupted flash writes.
