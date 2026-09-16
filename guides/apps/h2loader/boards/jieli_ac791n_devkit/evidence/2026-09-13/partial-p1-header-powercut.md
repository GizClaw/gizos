# Partial P1 header: physical power-loss recovery

Operator confirmed the requested power-off/on operation (not Reset) on
2026-09-13. UART capture independently records POWER ON after the fixture.

- Device: `3ce9e275d7aa`; UART `/dev/cu.usbserial-20131240`, 460800.
- Package: `6c92406e55ce0ea40e3c2efb6f36ccda46a9e32a4b1d7b8534da843c791a3ddd`, 917481 bytes.
- Image: `d9e3547897bd9e45faaab38d4ecbc721d69113cecc417b7d50be1617e4741848`, 929021 bytes.
- Host capture: `tmp/jieli/partial-p1-v2-powercut-retry-monitor.log` (local artifact).

Observed sequence:

1. `H2_JIELI_PARTIAL_P1_READY prefix=16 p2_crc=valid` through
   `00:39:04.470`: fixture readback confirms the first 16 P1 header bytes and
   erased remainder, with valid P2 CRC. The diagnostic hold survives the normal
   five-second writer timeout.
2. `system reset reason: POWER ON` after operator power removal.
3. `H2_JIELI_UPDATE_WRITE_DONE expected=929021 native=929021 result=0`.
4. `H2_JIELI_UPDATE_BURN call=0 pend=0 result=0`.
5. `H2_JIELI_BOOT_INFO result=0 base=0x4020 bytes=928957 version=26 logical=1`,
   followed by startup event 4/code 0 and SOFT reset reason.
6. Independent UART status succeeds: running/next partition 1, Stage invalid
   (cleared), last_result 0, both partition image identities equal the SHA above.

Result: PASS for recovery from this specific torn P1 header and safe retry to
P1 after physical power loss. This is not exhaustive coverage of every flash
bit pattern or an acceptance claim for Preference-write interruption.

During the extended diagnostic hold the SDK emitted
`UART0_CIRCULAR_BUFFER_WRITE_OVERLAY` warnings. These remain disclosed; normal
post-recovery UART status succeeds, but this test does not establish their root
cause or resolve logging robustness generally.
