# WebRTC test server

This host-only fixture provides isolated loopback HTTP signaling, STUN, and
authenticated TURN endpoints backed by Pion. Each `/offer` request owns one
PeerConnection and returns its opaque identifier in `X-H2-Session-ID`.
`/session/<id>/close` triggers a remote close,
`/session/<id>/channel-stats` exposes atomic created/opened/closed/current/max
DataChannel counters, and `/session/<id>/ice-pair` reports the actual selected
candidate protocol, type, TCP type, and observed UDP drop count from Pion's
`GetSelectedCandidatePair()`. `/turn-stats` exposes relay acceptance counters,
and `/shutdown` performs bounded process cleanup.

The server binds every listener to `127.0.0.1` on an ephemeral port. It does
not contact public signaling, STUN, TURN, media, or broker services. Passing
`X-H2-Relay-Only: 1` requires the submitted offer to contain a relay candidate.

`--ice-mode=udp|tcp|mixed|mixed-drop-udp` selects the process-wide Pion ICE
transport inventory. UDP and TCP modes expose one loopback mux, mixed exposes
both, and mixed-drop-udp keeps a real advertised UDP candidate while silently
dropping its inbound traffic so the client must fall back to TCP. Every mode
uses ephemeral HTTP, STUN, TURN, ICE UDP, and ICE TCP ports as applicable.

Run the fixture tests from this directory:

```sh
go fmt ./...
go mod tidy
bazel test //tools/webrtc-test-server:webrtc_test_server_test
go vet ./...
```

Run the PAL compatibility executable from the repository root:

```sh
bazel test --config=macos_arm64  //libs/pal/providers/desktop:h2peer_pal_pion_test
bazel run -c opt --config=macos_arm64 \
  //projects/e2e/targets/cc_binary/webrtc-performance:h2peer-performance -- \
  --profile=benchmark --runs=10 --transfer-bytes=10485760
```

Use the matching Bazel platform config on Linux. Each Desktop launcher starts
and reaps its own server binary; developers do not need to start this tool
manually. The H2Peer compatibility test keeps only UDP/TCP/TURN basic
interoperability, data and Opus echo, remote close, and reconnect. Long
transfer, concurrency, cadence, and throughput work belongs to the portable
performance App rather than `cc_test`.

The performance executable uses a binary `H2PF` fixture protocol on the
existing channels. It runs ten iterations on one UDP PeerConnection, compares
data-only with a loaded mix of three request-scoped channels, Packet/Event
traffic, a sequential 10 MiB download followed by a 10 MiB upload, and 20 ms
Opus RTP across both directions. The server validates
upload offsets and exact byte counts and produces a deterministic segmented
download; the client reports idle/loaded latency, upload/download throughput,
loaded-to-data-only throughput, and RTP arrival gaps.

Fixture logs are intentionally limited to mode, candidate family/port,
selected protocol/type, and counters. They do not print complete SDP, ICE/TURN
credentials, or application payloads. This is a host loopback interoperability
gate; it is not hardware runtime evidence for ESP-IDF or BK7258.

For an explicit DevKit LAN run, bind HTTP and STUN to the host interfaces and
advertise the host's test-Wi-Fi IPv4 address:

```sh
bazel-bin/tools/webrtc-test-server/webrtc_test_server_/webrtc_test_server \
  --listen=0.0.0.0:18080 \
  --stun-listen=0.0.0.0:3478 \
  --turn-listen=127.0.0.1:0 \
  --candidate-ip=192.0.2.10 \
  --ice-mode=udp
```

Replace the documentation-only `192.0.2.10` address with the operator host
address and use the same value for
the DevKit package's allowlisted endpoint and STUN build variables. LAN bind is
opt-in; the default remains loopback with ephemeral ports.
