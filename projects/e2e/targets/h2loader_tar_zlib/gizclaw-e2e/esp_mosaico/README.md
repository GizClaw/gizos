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
