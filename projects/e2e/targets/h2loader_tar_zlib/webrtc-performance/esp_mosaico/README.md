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

An optional private `--define=H2_WEBRTC_PERF_WIFI_BSSID_HEX=<12 hex digits>`
selects a unicast AP for a controlled comparison. It modifies only the runtime
STA configuration; saved credentials/settings remain unchanged. The app checks
the connected BSSID before admitting the workload and logs the actual channel
and RSSI. Omitting the define uses saved settings. Matching channels do not prove
that the host and board use the same AP. Keep actual AP identifiers in private
fixture configuration and record this condition with the result.

For delay attribution, `--copt=-DH2_PEER_RTP_TX_DIAGNOSTICS=1` records at most 256 RTP transport-send attempts per connection, including retries, with sequence/SSRC, return code and monotonic timestamps immediately before and after the transport call. It retains no payload and prints only at connection destruction. The overflow count makes truncation explicit; an unavailable timestamp remains `UINT64_MAX`. Pair it with `--copt=-DH2_PEER_RTP_RX_DIAGNOSTICS=1` and the actual peer timing receipts to distinguish submission, transport-call and receive gaps. These are software call timestamps, not over-the-air transmit timestamps. Both flags default off, and diagnostic runs do not qualify the default firmware.
