# AI Passport Loader layout

This is the product upgrade layout for the 8 MiB AI Passport. The canonical
machine-readable map is [partition.csv](partition.csv). Both the ordinary
Loader entry and `projects/h2loader/targets/h2_loader_tar_zlib/mfg/folotoy/ai_passport` select this same map through
`layout_files.partition`. The generic `esp32c3_chip` smoke layout is not used
for product installation, managed upgrades or recovery.

## Flash map

Offsets and sizes are physical Flash bytes; ends are exclusive. `K` and `M`
in the CSV mean 1024 and 1024 × 1024 bytes.

| Name | Type / subtype | Offset | Size | End | Purpose |
| --- | --- | --- | --- | --- | --- |
| `nvs` | data / nvs | `0x009000` | `0x006000` (24 KiB) | `0x00f000` | SDK NVS and legacy preferences |
| `otadata` | data / ota | `0x00f000` | `0x002000` (8 KiB) | `0x011000` | SDK OTA selection |
| `phy_init` | data / phy | `0x011000` | `0x001000` (4 KiB) | `0x012000` | PHY initialization |
| `h2loader` | app / ota_0 | `0x020000` | `0x200000` (2 MiB) | `0x220000` | P1, partition ID 1 |
| `app` | app / ota_1 | `0x220000` | `0x300000` (3 MiB) | `0x520000` | P2, partition ID 2 |
| `pref` | data / littlefs | `0x520000` | `0x040000` (256 KiB) | `0x560000` | PAL preferences, Loader metadata and MFG results |
| `coredump` | data / coredump | `0x560000` | `0x010000` (64 KiB) | `0x570000` | Reserved core-dump storage |
| `data` | data / littlefs | `0x570000` | `0x290000` (2,686,976 bytes) | `0x800000` | Shared `/dl` and `/data` filesystem, mounted under `/passport` |

The bootloader occupies the area below the partition table at `0x008000`;
the table has one 4 KiB sector ending at `0x009000`. The gap from `0x012000`
to `0x020000` is reserved. No recording operation writes either bootstrap area,
OTA selection, P1, preferences, core dumps or the data filesystem.

## Image limits and managed updates

- Ordinary Loader and MFG both have `role=h2loader`. Their ESP image must fit
  P1: at most `0x200000` bytes, including image headers, segments and trailers.
  Staging that image in the larger P2 slot does not increase the supported
  Loader limit: managed self-upgrade must still copy it back into P1.
- An ordinary `role=app` image may use the full `0x300000` bytes in P2. A valid
  App candidate, or an image currently running from P2, makes the recording
  backend unavailable. The recording window does not permanently shrink P2.
- Stage is a package file in the data-backed `/dl` filesystem, plus Loader
  metadata in preferences. The package must fit actual available filesystem
  space, including filesystem overhead and existing files; it does not use
  the recording window. A valid Stage blocks recording admission.
- Managed Loader updates write and verify P2, run it, copy the Loader image
  back to P1 and clear Stage after successful completion. Manual P1 recovery
  durably selects Loader command intent before seeding a changed identity,
  so stale Stage/P2 state cannot immediately overwrite the recovered image.

## Temporary AEC recording window

The recording backend borrows only the final `0x40000` bytes of an inactive P2
that contains a valid Loader candidate. This is not a separate partition.
Its P2-relative range is `[0x2c0000, 0x300000)`; with the map above its absolute
range is `[0x4e0000, 0x520000)`. The exclusive end is exactly the start of
`pref`, so it cannot extend into persistent preferences, core dumps or data.
A maximum-sized supported Loader ends at absolute `0x420000`, leaving a
`0x0c0000` gap before the recording window.

[The implementation](../../../../../../projects/h2loader/targets/h2_loader_tar_zlib/mfg/folotoy/ai_passport/main/recording.c)
checks space before recording or erasure:

1. P1 must be the running Loader partition; P2 must be inactive, have valid
   `role=h2loader` metadata and have no valid Stage. Recorded P2 image size
   must not exceed the recording start at relative `0x2c0000`.
2. The PAL disk must identify P2 as `app`, exactly `0x300000` bytes, with a
   4096-byte erase block and readable/writable/erasable flags. Write alignment
   must divide both the erase block and the encrypted-frame stride. The SDK
   must locate an inactive OTA_1 `app` partition of the same size.
3. `esp_image_verify()` independently verifies the actual P2 ESP image and
   requires a nonzero SDK image length no greater than `0x2c0000`. Valid
   metadata alone is insufficient to authorize erasure. Preparation provides
   a single-use permit; open checks current identities and geometry again,
   and repeats SDK verification if the permit is absent or changed.
4. The backend allows one or two recordings, nonzero frame counts and
   8-sample-aligned frames of at most 512 mono S16 samples. Each encrypted
   frame uses `samples * 2 + 16` bytes, including its AES-128-GCM tag.
   Per-recording space is rounded up to 4096 bytes; the full count must fit
   `0x40000` bytes. Oversized requests fail before any erase or write.

For the current two-recording, 96-frame, 512-sample profile:

```text
frame stride       = 512 * 2 + 16                       = 1,040 bytes
recording region   = ceil(96 * 1,040 / 4,096) * 4,096    = 102,400 bytes
both recordings    = 2 * 102,400                       = 204,800 bytes
window capacity    = 0x40000                           = 262,144 bytes
remaining capacity = 262,144 - 204,800                  = 57,344 bytes
```

The two allocated regions are absolute `[0x4e0000, 0x4f9000)` and
`[0x4f9000, 0x512000)`, both inside the verified unused tail. No image bytes
or partition boundaries move. Frames retain their full PCM payload and
independent authentication; keys and nonce prefixes live only in RAM.
Checked close zeroizes these secrets and erases the allocated recording
regions. Failed erasure retains retryable ownership and cannot be treated as
successful release. A reset loses the ephemeral key and does not imply that
an interrupted erase completed.
