# Mosaico webrtc-performance E2E

This managed S31 entry composes the existing portable `webrtc-performance` App, the
Mosaico PAL and board-owned dual CDC adapter. It owns its PSRAM entry task and
synchronous screen presentation; it changes no public PAL contract or reference
board launcher. CDC0 records diagnostics; CDC1 retains the public H2Loader
protocol. Screen position 18/19 shows startup, workload and admission.

Build `//projects/e2e/targets/h2loader_tar_zlib/webrtc-performance/esp_mosaico:package`
with `--config=esp32s31` and the pinned IDF 6.2. Install through the public Loader
on the directed device. Wi-Fi credentials are loaded from saved settings, not
checked into this target. Bind real results to source, configuration, package,
image and peer hashes. A build, missing peer, partial run or old replay is not a
qualification PASS. Preserve Loader/Stage and coredump evidence on recovery.

Supply real offer and STUN peers with `--define=H2_WEBRTC_PERF_ENDPOINT=<url>`
and `--define=H2_WEBRTC_PERF_STUN_URL=<stun-url>`. The documentation defaults
intentionally fail configuration. Existing power-save/BLE defines remain
available. One complete shared smoke workload records real upload/download,
loaded transport, request timing and audio gap metrics. Failure remains visible
and unconfirmed; a later retry must have a fresh independent receipt. Confirmation
follows actual workload success. Audio metrics are not acoustic qualification.
