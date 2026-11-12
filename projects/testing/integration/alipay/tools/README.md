# Local Alipay trace capture

This Go tool reads an existing H2Loader monitor stream or file. It does not access the serial port, reset/install a device, send SDK requests, contact a gateway or change a credential. Build and device lifecycle stay with H2Loader and the [standalone App](../README.md).

The standalone entry records numeric summaries for the selected [SDK adapter](../vendor_sdk/README.md) or [independent client](../../../../../libs/clients/alipay_independent/README.md). The collector also understands explicitly authorized historical private captures with `-private`; that flag does not enable capture in firmware. Keep monitor logs and output directories local.

```sh
bazel run --config=macos_arm64 //projects/testing/integration/alipay/tools:alipay_trace -- \
  -in /absolute/local/monitor.log -out /absolute/local/new-trace-directory \
  -firmware-git-sha <actual-build-commit> \
  -firmware-package /absolute/local/package.update.tar.zlib
```

The output directory must be new. `host.json` records collection time, host-supplied build SHA/dirty state and optional package digest. These fields describe the actual build supplied by the caller; they are not attested by firmware. Device `run.json` records runtime IDs, board/source, firmware version, the verified SDK manifest digest and actual observability limits. The current Firmware Info PAL exposes a version, not a source Git SHA; associate the build manifest/package checksum with H2Loader's before/after receipts and use `-firmware-dirty` if appropriate. No vendor SDK version string has been supplied, so its version remains unknown and its exact inputs are identified by the manifest.

Each runtime run/case gets `events.jsonl`, `run.json`, `integrity.json`, and indexed `call-*-conn-*-seq-*.bin` prefixes when private capture was enabled. Binary files preserve the observed bytes exactly. Their JSON indexes distinguish observed/captured lengths, full observed digest, captured digest, connection/call/request relation, boundary and truncation. They are ordered syscall fragments or formatted SDK messages, not reconstructed HTTP messages. Request identifiers refer to the observed outer SDK call; internal RPCs receive no invented IDs. An Operation-Type is reported only if the SDK actually logs that exact header.

For historical private captures containing ordered application-code fragments and a successful display result, the collector can emit `binding-code.txt` and, with `-qr-renderer`, `binding-code.svg`. The standalone summary capture cannot reconstruct a QR. Controlled host fixtures never establish vendor/device success.

The terminal diagnostics summary records the configured per-class sampling limit and seen/summarized counters. Sampled vendor logs and successful clock/allocation diagnostics remain distinct from unexpected dropped records; SDK/HAL results and other operations are not sampled. Capture limits, sequence checks, missing terminal detection and integrity status are enforced by the [collector](collector.go) and the public bounded ring. Drops or missing records fail collection; raw-prefix truncation remains explicit even when event metadata is complete. Export runs only after the SDK worker joins, outside callbacks and business timing. Standalone CASE/SUMMARY records, completed-display observations and the matching summary replay remain independent acceptance evidence.

The observable layers are product input/result/render, real SDK entry/return/reuse, formatted SDK logs, identity/secure-provider boundaries, DNS and socket IO beneath SDK TLS. No supported plaintext HTTP request/response hook, TLS handshake result or raw HTTP reassembly is available in this delivery. Socket captures may contain TLS records and cannot establish RPC contents, signatures or plaintext token provenance. Do not disable verification or treat a PCAP as sufficient to reproduce signing; future plaintext support must come from an actual SDK boundary.
