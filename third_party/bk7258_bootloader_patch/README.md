# BK7258 mapped AB bootloader patch

`mapped_ab_v1` adapts one pinned vendor AB bootloader so a Flash-AES trial image
is validated through the same A-mapped view it executes from, and fallback
reads the header and payload from the same partition.

- Source: `bk_avdk_smp_3.1.1`
  `cp/components/bk_libs/bk7258/bootloader/ab_bootloader/bootloader.bin`,
  18720 bytes, SHA-256 in `manifest.json`. The SDK appends its partition table
  to those bytes.
- Change: a four-byte branch at logical `0x2070`, a 64-byte descriptor at
  `0x7f00`, and the freestanding checker at `0x8000`. Only the CRC units holding
  those bytes are rewritten. The BootROM marker and log gate stay unchanged.
- Layout: CRC-enabled partitions with `primary_bootloader` at zero, followed
  directly by `primary_cp_app` and `primary_ap_app`. `s_app` is non-executable,
  starts after them and is at least as large. All sizes and offsets are multiples
  of 68 KiB inside 16 MiB. The vendor `appa` and `s_app` records must match.
- Refused: other pinned bytes, a payload with a different SHA-256, non-padding
  destinations, unsupported partition geometry, and a Loader image that fails
  the checker's RBL, vector or digest rules.

`apply.py` runs inside the native build. It has no key, device access or SDK
mutation, and the patched region stays plaintext until the Flash-AES packaging
step. This code does not depend on GizOS Core, PAL, boards or projects.

## SDK update

A new SDK bootloader, toolchain or checker change is a new contract:

1. Re-derive the entry, lookup address, partition record offsets and padding
   from the new binary.
2. Update `manifest.json` and the matching constants in `apply.py`, plus the
   addresses in `mapped_ab.c`, `mapped_ab.ld` and `contract.h`, and record the
   new payload SHA-256. An incompatible change also needs a new contract version
   in `contract.h`.
3. Requalify on hardware, including A fallback and an interrupted copy-back.
