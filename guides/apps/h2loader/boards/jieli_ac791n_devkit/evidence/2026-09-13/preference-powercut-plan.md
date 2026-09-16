# Preference power-loss acceptance: two injected programs recovered

Existing `test_jieli_pref_commit.py` replaces littlefs calls with stubs. It
checks call ordering and preservation of the old destination on API failures;
it does not exercise littlefs metadata recovery or real NOR interruption.

The physical fixture must use the board's existing Preference implementation
and a dedicated test namespace, not overwrite Loader control keys. Establish a
known committed old blob plus an unrelated sentinel, then replace the blob with
a distinct new value. Arm interruption only during that replacement.

Required interruption boundaries:

1. Temporary-file data/metadata programming before the replacement is durable.
2. Metadata programming within the `lfs_rename` replacement transaction.

At each boundary, a diagnostic-only hook in the real NOR program path must
produce and read back a partial program, then hold with a UART marker before
cleanup or further filesystem calls can change the evidence. A pause after an
entire transaction or in `pref_commit()` is not a write-interruption test:
`pref_commit()` performs no I/O in this backend.

The hook must be disabled by default and stay disarmed during boot, Loader
self-update, normal Preference writes, and recovery. The recovery boot must not
repeat the injection. Never format Preference automatically to hide a failed
mount; preservation of existing Loader state is part of acceptance.

After the operator removes power, capture POWER ON and verify through the same
PAL that the blob equals the complete old or new value (including length and
bytes), the unrelated sentinel survives, and subsequent replacement succeeds.
Also request Loader status to verify the active role, partition identities,
and absence of an unintended staged upgrade. Record image/package hashes,
interrupted program address/length/prefix, and each independent readback.

Status: first-program fixture built and installed through UART Loader;
physical power-cycle recovery PASS for this injected program. The separate
rename-transaction boundary also passed its injected program, as recorded below.

The diagnostic target is `loader_pref_powercut_package`. Its package is
918139 bytes, SHA-256
`06dfa3943a91c4e487da2e625d55d51a4925f330c5c00642de2902d7feb8a153`;
the image is 929821 bytes, SHA-256
`67c646dbbd6ce5a82fbbf537a3073b8b90f91f393b519d37db10fe2f03c2759b`.
UART reported successful stage receive and accepted `reboot upgrade`.
After installation, native boot info reports P1 base `0x4020`, version 28,
native length 929757. The diagnostic repeatedly reports:

```text
H2_JIELI_PREF_CUT_READY address=71b000 size=256 prefix=128
```

READY is emitted only after verifying the partial-program readback and that
the omitted half would change at least one flash bit. The old blob and sentinel
were read back before arming. This first eligible program is not yet classified
as file data versus metadata; it does not prove the separate rename boundary.
The operator confirmed power off for five seconds followed by power on.
Capture line 27283 reports `reset reason: POWER ON`; lines 27452–27453 report:

```text
H2_JIELI_PREF_RECOVERY complete=1 sentinel=1 retry=0 verified=1
H2_JIELI_PREF_TEST return=0 marker=1
```

Thus the recovered blob is a complete old-or-new value, the unrelated sentinel
survived, and a subsequent replacement plus full readback succeeded. After
stopping the monitor, an independent UART status request succeeded for device
`3ce9e275d7aa`: active role Loader, running/next partition 1, both partition
identities matching the image above, `stage_valid=0`, `last_result=0`.
Local capture: `tmp/jieli/pref-powercut-v1-monitor.log` (not committed).
The long diagnostic pause also produced SDK
`UART0_CIRCULAR_BUFFER_WRITE_OVERLAY` warnings; this test does not establish
general log-buffer reliability or explain those warnings.

Host simulations, API-error tests, and boot-header power-loss results cannot
substitute for these Preference-specific observations.

## Rename boundary: physical recovery PASS for the injected program

The first rename fixture completed without injecting (`injection_missed=0`),
so it provides no power-loss acceptance. Version 2 chooses a prefix within
the observed changed-byte range, instead of assuming changes span byte 128.
Both previously programmed and deliberately omitted changed bytes are required.

The version-2 package is 918794 bytes, SHA-256
`ae5c7fa62287081c69cdb45b1fa557ff7e77c11b8df5a5893ba0c0ff59d12cae`;
image is 930589 bytes, SHA-256
`e06de21c3b21eca04f4ea65d8062ea6a994fa601a885b5ea10395cbc434b90f6`.
UART stage receive succeeded and Loader installed it. The backend notification
brackets the actual `lfs_rename` call. Only while that notification is active
and the dedicated replacement is armed may the program observer inject.

```text
H2_JIELI_PREF_RENAME_PROGRAM address=71cc00 first=0 last=45
H2_JIELI_PREF_CUT_READY phase=rename address=71cc00 size=256 prefix=23
```

The partial 23-byte program was read back and matched the expected NOR contents.
The board paused before returning to littlefs. The operator then confirmed the
requested five-second power cycle. Capture line 1651 reports `POWER ON`, and
lines 1820–1821 report `complete=1 sentinel=1 retry=0 verified=1` and
`return=0 marker=1`. This proves a complete old-or-new blob, preserved sentinel,
and successful replacement/readback after this interrupted rename program.
An independent UART status request succeeded after stopping the monitor:
device `3ce9e275d7aa`, Loader in P1 with next P1, both partition identities
matching the version-2 image, `stage_valid=0`, `last_result=0`.
This is one concrete torn-program pattern, not exhaustive power-loss coverage.
Earlier recovery lines
in this capture belong to the previous diagnostic image during upgrade and
must not be counted as this rename test's recovery.
Local capture: `tmp/jieli/pref-rename-powercut-v2-monitor.log` (not committed).
