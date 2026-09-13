# UART lifecycle: candidate Loader copy failed

Source: `c904d81cc11900e7e2e72861de3ab483bc1f8dcf`.
Device: `3ce9e275d7aa`, UART `/dev/cu.usbserial-20131240`, 460800 baud.
Local captures: `tmp/jieli/uart-lifecycle-c904.json` and `.log`.

The current-source runner passed 16 cases and failed `install-loader`.
This is not full lifecycle acceptance and supplies no fresh BLE acceptance.
Candidate package SHA-256:
`70d37a1169125d2bc7930e337f72d6ef2470bae8177c79cfd2f941d98f67b659`.
Candidate image SHA-256:
`ffc162098f7564c28b949f6182bd839f75a3d17fa9ba4f3063914b92746bf176`.

The candidate booted partition 2, confirmed and published its header, then
began copying to partition 1. The first source read reported:

```text
H2_JIELI_IMAGE_READ_SD_ERROR step=stat partition=2 offset=0 bytes=65536 read=0 rc=-8
```

After the user's restart, UART status still responded with the candidate
image running in partition 2, partition 1 invalid, and `last_result=-8`.
Do not interpret this as a successful self-update or an inaccessible board.

## Diagnosis and pending verification

The old diagnostic conflates a failed stat with a successful stat whose
`is_dir` field is true: both produce the same NOT_FOUND error and step label.
The latter now has the distinct label `stat-directory`.

In the built candidate ELF, SDK `fdir_exist` at `0x20157fe` calls `fopen` at
`0x20158a4`, passing the mode string at `0x209d6dd` (`"r"`), and returns 1
when that open succeeds. It does not inspect FAT directory attributes.
The PAL added in `a4ab5f6b` incorrectly used that result to classify files
as directories. The proposed fix reads `fget_attr` and `F_ATTR_DIR` instead.

The shared filesystem E2E case now checks a newly written regular file's
stat type and exact byte size. The previous JieLi launcher only selected Core
and Wi-Fi, so its nine passing cases did not cover this regression.
The launcher now also selects the standalone `H2_PAL_E2E_SUITE_FILESYSTEM`,
reusing that public case without requiring host network fixtures.
Unit tests execute this suite with correct attributes, a false directory
classification, and an incorrect size; the latter two must fail while still
cleaning up the fixture files. Both PAL test targets passed locally.
Host unit-test success does not prove SDK behavior on the board.
The corrected Loader still requires hardware installation and lifecycle
retesting; preserve the running candidate until recovery is prepared.
