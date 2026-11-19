# Local Alipay trace capture

This Go tool reads an existing H2Loader monitor stream or file. It does not access the serial port, reset/install a device, send SDK requests, contact a gateway or change a credential. Build and device lifecycle stay with H2Loader and the [standalone App](../README.md).

The standalone entry records numeric summaries for the [self-implemented client](../../../../../libs/clients/alipay/README.md). Explicitly authorized private captures require `-private`; that flag does not enable capture in firmware. Keep monitor logs and output directories local. On Windows, use a parent directory whose inherited access control restricts the capture to authorized users.

```sh
bazel run --config=macos_arm64 //projects/testing/integration/alipay/tools:alipay_trace -- \
  -in /absolute/local/monitor.log -out /absolute/local/new-trace-directory \
  -firmware-git-sha <actual-build-commit> \
  -firmware-package /absolute/local/package.update.tar.zlib
```

The output directory must be new. `host.json` records collection time, host-supplied build SHA/dirty state and optional package digest. These fields describe the actual build supplied by the caller; they are not attested by firmware. Device `run.json` records runtime IDs, board/source, firmware version and actual observability limits. The current Firmware Info PAL exposes a version, not a source Git SHA; associate the build manifest/package checksum with H2Loader's before/after receipts and use `-firmware-dirty` if appropriate.

Each runtime run/case gets `events.jsonl`, `run.json`, `integrity.json`, and indexed `call-*-conn-*-seq-*.bin` prefixes when private capture was enabled. Binary files preserve the observed bytes exactly. Their JSON indexes distinguish observed/captured lengths, full observed digest, captured digest, connection/call/request relation, boundary and truncation. They are ordered syscall fragments or formatted SDK messages, not reconstructed HTTP messages. Request identifiers refer to the observed outer SDK call; internal RPCs receive no invented IDs. An Operation-Type is reported only if the SDK actually logs that exact header.

For historical private captures containing ordered application-code fragments and a successful display result, the collector can emit `binding-code.txt` and, with `-qr-renderer`, `binding-code.svg`. The standalone summary capture cannot reconstruct a QR. Controlled host fixtures never establish vendor/device success.

The terminal diagnostics summary records the configured per-class sampling limit and seen/summarized counters. Sampled vendor logs and successful clock/allocation diagnostics remain distinct from unexpected dropped records; SDK/HAL results and other operations are not sampled. Capture limits, sequence checks, missing terminal detection and integrity status are enforced by the [collector](collector.go) and the public bounded ring. Drops or missing records fail collection; raw-prefix truncation remains explicit even when event metadata is complete. Export runs only after the SDK worker joins, outside callbacks and business timing. Standalone CASE/SUMMARY records, completed-display observations and the matching summary replay remain independent acceptance evidence.

The current observable layers are product input/result/render, client entry/return/reuse, HTTP result metadata and provider diagnostics. The standalone configuration emits no packet bytes. Evidence must identify its actual boundary; a transport or processing result alone is not a merchant receipt. Keep TLS verification enabled and private payment material out of ordinary logs.
