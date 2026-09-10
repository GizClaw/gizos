# BK7258 fixed-address Flash XIP bench experiment

This experiment checks whether the existing H2Loader, selected as the native
boot target, can execute a separately linked program at a different address in
the same SiP Flash without selecting the BK B slot. It is not a production
CP/AP application handoff or an OTA implementation.

The bench Loader AP launcher included `fixed_xip_probe.h`, kept next to this
README; no production target includes it. At the start of the Loader entry
task, before Runtime/storage initialization, it checked that the current
native partition is A and validated a small probe header. Ordinary B images
lack this header and were not called.
The hook neither programs Flash nor changes OTA flags.

## Payload

`payload.c` is independently compiled and linked, without SDK or Loader
symbols. It reads its actual PC and an absolute-address constant, writes its
own UART0 messages by MMIO, and returns to the Loader after three iterations.
The Loader reports the returned evidence and the native partition again.
The payload uses the caller's stack; interrupts on the calling AP core are
masked during the short call. CP and the existing SDK runtime remain present.
This deliberately tests fixed-address execution before attempting to replace
the runtime, vector table, stacks, or CP firmware.

The physical start is `0x003b8000`, the existing `s_app` start. With BK's
32 data bytes + 2 CRC bytes format, its XIP base is `0x02380000` and entry
is `0x02380101` (Thumb). No same-address remapping is requested.

```sh
python3 projects/h2loader/experiments/bk7258-fixed-xip/build_payload.py \
  --toolchain /absolute/path/to/toolchain/bin --output /absolute/path/to/output
```

The script emits ELF, map, unencoded binary, CRC-encoded binary and a manifest.
It does not access hardware. The small program's entry is deliberately not an
RBL image and must not be installed through the normal managed App updater.

## Hardware procedure

1. Identify the BK7258 by a fresh managed `status`, not a saved serial name.
2. Back up the complete Flash and retain the original firmware packages.
3. Install the experimental Loader and verify native startup remains A.
4. Write the CRC-encoded payload to the specified physical address, retaining
   the original sector. Do not overwrite the Loader or calibration regions.
5. Capture UART0 and UART1. Require payload PC/constant addresses in its linked
   range, the expected constant and three iterations, and native partition A
   before and after the call.
6. Reset and repeat, proving the Loader starts before the payload again.
7. Restore the backed-up device state and verify managed communication.

An optional build-only `--define=h2_bk7258_tmpdir=/absolute/path` selects the
native SDK temporary directory when the system volume lacks space. It does not
change firmware configuration.

## Limits

A passing probe establishes fixed-address SiP Flash XIP execution. It does not
establish a complete SDK App startup, CP/AP replacement, interrupt operation,
power-failure-safe updates, an asymmetric production partition map, or QSPI XIP.
Those require subsequent experiments. The existing equal-sized partition table
is retained here to make recovery reproducible; the payload itself occupies
only a small independently linked image within it.

## Bench result — 2026-09-10

Based on `origin/main` at `90fb3b83`, with SDK
`aa5df964b0f64924ee6d0d2ffd6c3ca6ed59f9ca`. Both UARTs were identified using
managed Loader status and observed reset logs. A complete 8 MiB backup was
obtained before writing. The existing native bootloader was retained.

Loader `0.1.43-fixed-xip` executed the payload at the end of its startup worker;
UART0 captured the independently linked program on two hardware-reset runs.
Managed status afterwards reported Loader, running partition 1, next partition
1, and an empty stage. Version `0.1.44-fixed-xip` moves the same experiment to
the early Loader entry and uses direct UART0 output for the caller as well.
Two CEN reset runs of this version produced the same complete trace. Managed
status afterwards again reported A/current, A/next and stage empty. The trace
establishes the call and return without a native bank switch:

```text
H2_FIXED_XIP loader_partition=0 base=02380000 magic=58324648
H2_FIXED_XIP jump=02380101 no_ota_switch=1
H2_FIXED_XIP_PAYLOAD pc=0238010a rodata=023801f4 value=72585849
H2_FIXED_XIP_PAYLOAD pc=0238010a rodata=023801f4 value=72585849
H2_FIXED_XIP_PAYLOAD pc=0238010a rodata=023801f4 value=72585849
H2_FIXED_XIP returned pc=0238010a rodata=023801f4 value=72585849 iterations=3 partition=0
```

The SDK numbers A as 0; the H2Loader protocol numbers it as 1. Payload CRC image
size is 544 bytes; SHA-256 is
`67b7955ae28f3dbcd3e19e3f7dad2f33f7068d565eb52d12e78cbf97a18b5f7d`.
Filtered UART evidence is retained in `evidence/`. Full Flash backups and raw
logs remain outside Git because they contain device state.

Both native firmware builds passed, as did `bk7258_runner_test` and
`cp_startup_contract_test`. The current Loader CP and AP raw images together
are about 2.35 MB, before Flash CRC/alignment overhead. A 2 MiB allocation for
this existing Loader therefore needs size reduction; it is not established by
this small payload test.

The hook was enabled only in the recorded bench builds. It is no longer
called by the current Loader launcher.
It is not a change to the production partition contract. B metadata still
reflects the old App while its first sector contains the test payload; never
request a normal native B boot in this temporary state. The test does not
remove ordinary Loader/SDK confirmation logic for A; it only avoids selecting
or confirming B to execute this payload. Reset testing uses CEN/RTS and does
not constitute a physical power-disconnection test.

## Recovery

The complete original 8 MiB Flash snapshot was restored after testing. The
UART0 programmer completed successfully. Fresh UART1 status then reported
`0.1.42-bk-e2e`, original image checksum
`a75ea290b5284030b5a62014859f7ed00fa8f2220384abaebec1bbe7bcb8fce4`,
running/next partition 1, Loader intent and an empty stage. Loader intent is
retained for bench access (the initial status before entering Loader used auto).
The original native B App and device configuration were included in the restore.
See `evidence/restored-status.txt`. No experimental firmware remains installed.
