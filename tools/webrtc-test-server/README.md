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
bazel test --config=macos_arm64  //pal/providers/platforms/desktop:h2peer_pal_pion_test
bazel run -c opt --config=macos_arm64 \
  //projects/testing/targets/cc_binary/webrtc-performance:h2peer-performance -- \
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

PAL conformance additionally supports explicit out-of-band DataChannels. Supply
all four `X-H2-Negotiated-ID`, `X-H2-Negotiated-Label`,
`X-H2-Negotiated-Ordered`, and `X-H2-Negotiated-Reliable` headers on `/offer`.
The fixture creates the matching Pion channel before applying the offer; no
DCEP is used for this channel. Missing headers retain ordinary in-band behavior.

`/session/<id>/authentication-witness` exposes the first fatal alert received by
that session's Pion DTLS logger as a typed protocol error, including its numeric
level and description. A certificate-rejection verdict requires fatal alert 42
or 46 and zero opened channels. Timeouts, generic handshake failures, and error
strings are not authentication evidence. Each session has a distinct witness.
This endpoint helps browsers that expose DTLS failure state without RTCError
fingerprint details; it never changes certificate verification behavior.

`/session/<id>/media-stats` reports received and echoed RTP packet counts,
input sequence gaps, deliberate drops, and echo-loop termination counters.
Periodic `H2_WEBRTC_TEST_SERVER_MEDIA` lines retain the same session identity.
The conformance-only `POST /session/<id>/media-fault` accepts `drop-one` (drop
the second subsequent packet) or `drop-all`. The hardware fixture never enables
these controls. Host stability regressions use them to prove that an isolated
RTP loss is reported and can recover, while 10 seconds without successful media
still fails qualification. Sequence tags in synthetic Opus distinguish unique
end-to-end echoes from duplicates; this is protocol liveness, not audio output.

## Optional RTP timing observations

`--rtp-timing-capacity=N` opts into a separate in-memory recording for each session. The default is `0` (disabled); valid enabled capacities are `1` through `65536`. For example, `--rtp-timing-capacity=4096` retains about 82 seconds at 50 packets per second. The server preallocates each buffer before accepting media, keeps the first N input packets, and counts subsequent packets in `overflow` without growing or overwriting the buffer. The disabled echo path takes no new timing samples and allocates no recorder or record storage. Recording adds no packet log output and does not change the RTP echo or fault controls.

`GET /session/<id>/rtp-timing` returns a coherent copy with `session_id`, `enabled`, `complete`, `capacity`, `observed`, `overflow`, and `records`. While active, a record can have outcome `received` and no completion time. Final outcomes are `echoed`, `dropped` (an explicit fixture fault, with no echo timestamps), and `write_error` (without error text). Unknown, expired, or evicted recordings return 404; other methods return 405. A live disabled session returns `enabled: false` and an empty record list. No HTTP authorization, bind, or session-ID boundary changes; protect opt-in LAN access in the same way as the existing session endpoints.

Each record contains only `input_sequence`, `input_ssrc`, `input_payload_type`, `input_length_bytes`, `receive_ns`, optional `echo_submit_ns` and `echo_complete_ns`, and `outcome`. Length is the input RTP packet's marshaled byte length, including its RTP headers and padding, without UDP/IP overhead. Input SSRC and PT describe the received packet, not the echo: Pion binds the echoed SSRC/PT to the negotiated local track. The existing echo preserves the source sequence, RTP timestamp, marker, and payload. Recording never retains RTP payloads, RTP extensions, SDP, credentials, or keys.

All three timing values use Go's monotonic clock as elapsed nanoseconds since that session's recorder was created, before answer generation. `receive_ns` is sampled under the recorder lock just after a successful Pion `TrackRemote.ReadRTP()` return; it is an application receive observation after Pion processing and any Pion buffering. `echo_submit_ns` is sampled immediately before calling `TrackLocalStaticRTP.WriteRTP()`, and `echo_complete_ns` just after it returns (including a failed return). Recorder locking and snapshot copies can add observer overhead. These are Pion API boundaries, not socket receive or actual UDP wire-send timestamps, and session clocks cannot be directly compared with another process or device clock.

At most eight recordings, including pending offers, live sessions, and completed sessions, hold buffers at once (roughly 32 MiB of record storage at maximum capacity, excluding response copies). New offers evict the oldest completed recording when necessary; if all eight are active, offers fail with the existing 400 response for answer-creation errors. Failed offers release their reservation. Completed buffers expire five minutes after completion, with expiry enforced on reads/new offers and the existing 15-second cleanup tick. Eviction and expiry release the buffer even if the original session object is still live. Explicit `POST /session/<id>/close` removes the ordinary session but joins its echo workers and leaves the completed recording retrievable; a Pion closed state or the existing 30-minute session cleanup also completes recording. With timing enabled, a terminal Pion Failed state asynchronously closes its PeerConnection to unblock RTP I/O, then joins echo workers and completes the recording. The completed recording remains queryable and eligible for the same eviction/expiry policy; an in-flight echo prevents completion and buffer release until it returns. Disconnected does not close the PeerConnection or complete recording, so the connection can recover. Timing disabled preserves the existing connection lifecycle. Process shutdown clears all recordings. No files are written.

The existing `download_backpressure_timeout` line retains `buffered` and samples `cwnd`, `rwnd`, `mtu`, `srtt_ms`, `bytes_sent`, and `bytes_received` once on timeout using the pinned Pion `SCTPTransport.BufferedAmount()` and `Stats()` APIs. Byte totals are association traffic counters, not acknowledged-byte counts. Stats and buffered amount are separate synchronized getter samples, not an atomic SCTP transaction. The pinned WebRTC v4.2.18 / SCTP v1.11.1 interfaces expose no public ACK-progress getter through the transport; `UNACKData` exists in the public stats type but is not populated by `Stats()`, so it is deliberately omitted. No packet logging or protocol changes are added. These observations do not establish a common cause for audio latency and SCTP stalls; hardware qualification remains separate.
