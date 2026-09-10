# AMOLED migration status

The selected architecture is Lua gameplay, vector/path artwork, analytic scene lights and the reviewed streaming retro synthesizer. The current macOS desktop implementation and acceptance evidence are documented in [VECTOR-DESKTOP.md](VECTOR-DESKTOP.md). CoreGraphics remains the accepted desktop backend. A portable software backend and a private Zero ESP32-S3 standalone entry are described in [ZERO-STANDALONE.md](ZERO-STANDALONE.md); device performance and stability acceptance remain pending.

## Historical device findings

### Current vector build (2026-09-10)

The device at `/dev/tty.usbmodem4101` answers H2Loader with `board=amoled`,
`target=esp32s3`, and `device_uid=30eda0ae0bf2`. Its active checksum still matches
the historical particle diagnostic below. The vector build has not been installed.

The `0.1.0-dev` pure-vector firmware builds successfully with
`--define=h2_qi_duel_software_vectors=true`. Its entry now starts normal pairing
and battle mode, with management-advertising pause/resume hooks. The target
requires that flag and is marked `no-release` pending performance acceptance.

| Capacity check | Before optimization | After optimization |
| --- | ---: | ---: |
| Application image | 5,514,624 bytes | 3,233,040 bytes |
| Remaining space in 8 MiB app partition | 2,873,984 bytes | 5,155,568 bytes |
| Compressed update package | 4,680,113 bytes | 2,398,768 bytes |

The update package is 48.75% smaller, but still exceeds the 2,097,152-byte download
partition by 301,616 bytes, before allowing LittleFS overhead. It has not been
transferred to the device.

Optimization uses `--define=h2_qi_duel_screen=amoled` to exclude 16 H106-specific
clash/fade frames, and H2VG v2 opcode 14 for lossless coordinate deltas. Whole-pixel
polygons use unit 4; quarter-pixel polygons retain unit 1. Zigzag varints preserve
every coordinate exactly. Both backends bound varints to three bytes, validate
the original signed-16-bit coordinate range and reject truncated or noncanonical
input. Decoding adds no persistent buffers or allocations. The component bank
shrinks from 3,272,053 to 990,518 bytes.

The original resource bank and SVGs remain the source reference. The build packer
round-trips every selected frame back to the original command bytes before
embedding. Rendering all 170 original component frames before/after encoding
produces byte-identical pixels in both CoreGraphics and the software backend;
see `validation/amoled-vector/packed-frame-comparison.json`. This comparison proves
encoding fidelity; it does not replace device performance or panel acceptance.

The current Loader stages the complete package at `/dl/update.tar.zlib`. AMOLED
maps `/dl` to internal LittleFS; its provider does not currently mount an SD card.
Application capacity therefore does not imply managed-update capacity. Storage
direction must be selected before installation: further geometry size reduction,
SD staging support, or a separately reviewed partition migration. No device,
partition, or SD writes were performed during this vector build.

The baseline package and native debug files are copied to
`build/qi-duel-amoled-vector/`, and the optimized build to
`build/qi-duel-amoled-vector-optimized/`. Exact hashes, package verification, and
the capacity gates are recorded in `validation/amoled-vector/build-report.json`
and `validation/amoled-vector/optimization-report.json`. Previous software-renderer host
comparisons do not establish AMOLED device frame rate or stability; both remain
unverified.

### Retired raster investigation

The investigation used `/dev/tty.usbmodem5101`, an ESP32-S3 AMOLED board reporting
16 MiB Flash, 8 MiB PSRAM and an 8 MiB application partition. The old unmodified
raster application exceeded image limits. Even a compact raster experiment reached
13,283,344 bytes and failed the application partition check. No partition changes
were made. Particle-only experimental firmware measured roughly 0.1–1 FPS.
These measurements concern retired implementations, not the current vectors.

Historical frame manifests, CRC comparisons and memory/coredump snapshots remain
in `validation/amoled/`. Detailed temporary logs were stored under
`/tmp/qi-duel-amoled-baseline/` and may expire. The last recorded hardware image was
a particle diagnostic; source cleanup does not change the firmware on the device.

## Removed experiments

The cleanup removes the particle-only boot/replay profile, renderer profiling,
optional capsule/tapered-path backend, offline mip format H2M8, derived-tint format
H2DT and their packers/tests. These paths were superseded by the no-texture
requirement and are no longer supported build modes. Recorded evidence is kept
for context; old flags and probe commands must not be used for new builds.

## Remaining implementation and validation

- Optimize the portable vector backend for ESP32, then measure decoded geometry and peak heap against real limits. The first software backend passes initial host image comparisons but has not met the 30 FPS requirement.
- Use the normal playable startup and capture device-submitted RGB565 frames at
  the same timestamps as desktop for pairing, all skills, close-ups and results.
- Compare every frame against the accepted desktop and check actual 30 FPS
  presentation deadlines separately from capture overhead.
- Exercise input, BLE and audio together; repeat rounds/restarts and inspect heap,
  stack margins, watchdogs and coredumps. Inspect the physical panel as well.

The original raster resources remain for visual reference and the existing
non-vector build path. Their retention does not select them for final firmware.
No flashing, SD writes or partition changes are part of this source cleanup.
