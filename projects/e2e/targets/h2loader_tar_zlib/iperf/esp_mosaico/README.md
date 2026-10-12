# Mosaico iperf E2E

This managed S31 entry composes the existing portable `iperf` App, the
Mosaico PAL and board-owned dual CDC adapter. It owns its PSRAM entry task and
synchronous screen presentation; it changes no public PAL contract or reference
board launcher. CDC0 records diagnostics; CDC1 retains the public H2Loader
protocol. Screen position 17/19 shows startup, workload and admission.

Build `//projects/e2e/targets/h2loader_tar_zlib/iperf/esp_mosaico:package`
with `--config=esp32s31` and the pinned IDF 6.2. Install through the public Loader
on the directed device. Wi-Fi credentials are loaded from saved settings, not
checked into this target. Bind real results to source, configuration, package,
image and peer hashes. A build, missing peer, partial run or old replay is not a
qualification PASS. Preserve Loader/Stage and coredump evidence on recovery.

The client uses the unchanged ordered TCP/UDP/SCTP matrix. Supply an actual
server with `--define=H2_IPERF_SERVER=<host>` and retain both endpoint records.
The default documentation address intentionally fails configuration. Existing
power-save/BLE/server-role defines remain available. Client confirmation follows
matrix success and SCTP cleanup; server role is a serving peer fixture, not a
completed client matrix. Report real rates without inventing a minimum threshold.
