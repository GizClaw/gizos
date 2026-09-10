# Zero ESP32-S3 standalone Qi Duel

The private `zero_esp_v3_0` entry links Qi Duel, its Lua runtime, the H2VG software renderer and board services into one independent H2Loader App. It does not link H106 Main. The public game implementation remains in GizOS; the board, launcher and partition layout remain in Firmwares.

## Local build

Run from the sibling Firmwares checkout, with its existing ESP-IDF 6.0 installation configured. The local module override selects the current GizOS working branch without changing the private repository's pinned revision.

```sh
bazel build -c opt --config=esp32s3 \
  --override_module=gizos=/absolute/path/to/gizos \
  --define=h2_qi_duel_software_vectors=true \
  //projects/h106/targets/h2loader_tar_zlib/qi_duel/zero_esp_v3_0:package
```

The entry requires the software-vector setting. It cannot silently select the legacy raster resources. Lua source and the accepted H2VG/H2VP geometry are embedded in `app.bin`; SVG sources, old textures and prerecorded PCM are not packaged. Retro audio is synthesized at runtime. The final H2Loader archive is `.update.tar.zlib`; partition capacity is checked against the uncompressed `app.bin`, not the compressed archive.

The board layout remains unchanged: 16 MiB internal Flash, 3 MiB Loader and 8 MiB App. This build does not flash the attached `/dev/tty.usbserial-510` device or modify external Flash.

## Validation boundary

The portable backend supports the accepted path commands, cubic curves, ellipses, affine transforms, nonzero fills, round strokes, gradients and clipping. Per-call variable workspace is limited to 4 MiB, plus small fixed tables and a scanline buffer; the caller owns the transient RGBA output. Nothing is cached inside the renderer between calls. The existing Lua canvas owns its bounded decoded-path and rendered-component caches.

The initial checks compare 177 vector streams and 110 native RGB565 H106 scene frames against the accepted CoreGraphics implementation. The comparison metric excludes empty background pixels. The recorded worst component RGB MAE is 3.77/255 and the worst full-scene RGB MAE is 2.95/255. Reports are in `validation/zero-standalone/`.

This establishes initial drawing compatibility, not hardware acceptance. The first software backend is slow: complex clash probes on the host observed about 1.5–2.1 FPS. It has not met the 30 FPS gate. Hardware peak RAM, PSRAM XIP overhead, BLE/audio concurrency, watchdogs and repeated-round stability have not been validated. A successful package build must not be described as a playable or production-ready device release. The private entry is excluded from firmware release with `no-release`.

## Measured build

The initial `0.1.0-dev` build produces a 5,558,096-byte `app.bin` (5.30 MiB), leaving 2,830,512 bytes (2.70 MiB) in the existing 8 MiB App partition. Its update archive is 4,705,999 bytes (4.49 MiB). The archive's `app/esp/app.bin` was decompressed and checked byte-for-byte against the native output; the generated partition table was checked separately. Exact hashes and validation status are recorded in `validation/zero-standalone/build-report.json`.
