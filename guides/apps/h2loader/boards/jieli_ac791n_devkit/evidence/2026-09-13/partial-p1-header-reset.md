# Partial P1 header: reset recovery only

The operator explicitly confirmed pressing Reset, not removing power. This
attempt must not be counted as physical power-loss acceptance.

- Device: `3ce9e275d7aa`, UART `/dev/cu.usbserial-20131240`, 460800.
- Package SHA-256: `6c92406e55ce0ea40e3c2efb6f36ccda46a9e32a4b1d7b8534da843c791a3ddd`.
- Image SHA-256: `d9e3547897bd9e45faaab38d4ecbc721d69113cecc417b7d50be1617e4741848`.
- Diagnostic logged `H2_JIELI_PARTIAL_P1_READY prefix=16 p2_crc=valid`
  repeatedly through uptime `00:35:08.670`. The normal burn timeout did not
  remove the partial header as in the first diagnostic attempt.
- The original host monitor terminated; the intervening startup log was not
  captured. A reopened UART monitor and a separate status request succeeded.
- Status reported `running_partition=1 next_partition=1 stage_valid=0
  last_result=0`, with both partition identities matching the image above.

This supports recovery after reset from the partial-header fixture. It does not
provide uninterrupted ROM-selection logs or evidence of physical power removal.
Physical power-loss recovery and Preference-write interruption remain pending.
