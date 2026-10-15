# Mosaico gizclaw-e2e E2E

This managed S31 entry composes the existing portable `gizclaw-e2e` App, the
Mosaico PAL and board-owned dual CDC adapter. It owns its PSRAM entry task and
synchronous screen presentation; it changes no public PAL contract or reference
board launcher. CDC0 records diagnostics; CDC1 retains the public H2Loader
protocol. Screen position 19/19 shows startup, workload and admission.

Build `//projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/esp_mosaico:package`
with `--config=esp32s31` and the pinned IDF 6.2. Install through the public Loader
on the directed device. Wi-Fi credentials are loaded from saved settings, not
checked into this target. Bind real results to source, configuration, package,
image and peer hashes. A build, missing peer, partial run or old replay is not a
qualification PASS. Preserve Loader/Stage and coredump evidence on recovery.

Use the shared explicit `app_config_fixture_*` build settings for a controlled
real E2E service, registration identity, runtime profile, known AppConfig value,
HTTPS objects and NTP. Empty service settings fail before business mutations and
retain the public recovery channel. Never commit tokens or private build files.
The default selects the reference full business registry; firmware/voice/
concurrency filters remain explicit subset profiles. Exact executing ledger,
nonce/CRC, cleanup, zero retained resources and actual confirmation gate admission.
The prerecorded voice fixture reports `physical_audio=0`: business voice success
does not qualify microphone/speaker hardware. Selecting physical audio fails
explicitly as unsupported in this entry; separate board Audio qualification is
required. No AEC or S31 ABI is inferred.

## Reference-board business qualification

Reuse the shared App and fixture contract used by AMOLED and BK7258. Their
[historical qualification](../../../../apps/gizclaw/evidence/sdk0232-pal32-qualification.json)
used the controlled Dev endpoint `edge-bj-01.dev.gizclaw.com:9821` and profile
`gizos-e2e-626`. That receipt belongs to its recorded source, SDK, configuration
and devices; it neither supplies credentials nor qualifies this Mosaico image.
Verify the selected test deployment and profile before a new live run.

The full catalog runs `resource`, `device-api`, `connectivity`, `rpc`, `firmware`,
`voice`, `concurrency` and `service`. Local fault-injection tests exercise error
and cleanup paths; a local HTTP/Pion fixture cannot replace the registered
backend, Workspaces, social resources, Firmware metadata or Voice workflow.

Set the existing `//projects/e2e/apps/gizclaw:app_config_fixture_*` flags in a
private local Bazel rc file. The reference-board inputs are:

| Flag suffix | Required input |
| --- | --- |
| `endpoint`, `token` | Controlled service endpoint and its E2E registration token |
| `profile`, `key`, `value` | Exact test RuntimeProfile and seeded AppConfig key/bytes |
| `device_api_url`, `audio_url` | Known HTTPS Device API and audio fixture objects |
| `time_server` | Reachable fixture NTP hostname or IPv4 address |
| `physical_audio` | `false` for this software business voice entry |

ESP uses its existing certificate bundle; do not disable TLS verification.
Wi-Fi settings remain on the board. The rc file, token-bearing generated source,
firmware packages and raw business records are private test inputs and outputs.

```sh
bazel --bazelrc=.bazelrc --bazelrc=/absolute/path/to/private-fixture.bazelrc \
  build --config=esp32s31 \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/esp_mosaico:package
```

Install the frozen package through the public Loader and capture the current
execution ledger, nonce/CRC, actual cleanup/retained/confirmation and image
identity. Repeat on an independent boot. Audit the full ledger with the existing
`//projects/e2e/apps/gizclaw:api_coverage` command, `--platform=esp_mosaico` and
that run's actual endpoint/backend/profile/controller exit status. The current
inventory requires all 228 APIs; historical 227-API receipts do not replace it.
Other-board, incomplete and failed ledgers remain invalid. Missing private
fixtures are configuration failures, not skipped or passing business cases.

An explicit `app_config_fixture_root_ca_hex` may bind a private test CA. The
Mosaico artifact owns a separate coreHTTP provider with REQUIRED verification;
empty input retains the borrowed BSP certificate-bundle provider. The owner and
API stay alive for the entire App/command recovery lifetime, including failed
or retained-resource runs. A failed Runtime initialization destroys the unused
owner. Certificate chain, hostname, dates and actual object hashes still require
live verification. These settings change no production or reference-board trust
policy. A locally created token/profile/AppConfig and HTTPS/NTP fixture does not
supply ASR/TTS provider credentials or qualify the full eight-case business run.
