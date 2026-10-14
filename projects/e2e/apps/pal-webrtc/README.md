# PAL WebRTC E2E

This portable qualification App consumes the public WebRTC PAL and a launcher-owned Pion fixture. It is separate from the WebRTC throughput benchmark. The production header, independent inventory and stable case registry define coverage: all 13 peer/channel operations, the read/write Track callbacks, owned event release, custom allocator lifecycle and every DataChannel configuration field.

The current registry has 43 mandatory cases. It checks default and legacy creation, allocator rejection at the first four allocation cuts and ownership, nonblocking/positive/negative polling, borrowed ICE/SDP/channel-label/message inputs, malformed SDP, all four ordered/reliable channel combinations, an explicitly requested stream ID, binary/UTF-8/empty/bounded messages, Opus direct send through the 1275-byte packet limit and Track delivery, WOULD_BLOCK retry with WRITABLE, quiescent Track detach, owned Opus events, channel/remote close, event retention through peer close, idempotent release, resource churn and rejection of a deliberately incorrect DTLS fingerprint. A missing operation blocks qualification; a failed case blocks dependent cases. No SKIP/BLOCKED result is counted as PASS.

The App borrows Runtime and fixture inputs and owns its peers, channels, events and diagnostic allocator state. Track callbacks are nonblocking and do not call peer control functions. Channels are never used after close; source pointers on retained events are identities only. The custom allocator remains alive through release of every event; the harness fails and retains its backing context if a provider does not quiesce. Tagged synthetic Opus packets test encrypted RTP transport, not a physical microphone, speaker or codec quality. The public contract has no video interface, so video is outside this gate.

Host tests use a separately implemented Pion peer with real ICE/DTLS/SRTP/SCTP. The launcher owns fixture startup, network binding and cleanup. Browser C runs in a pthread Worker and negotiates with the same Pion fixture through a same-origin signaling proxy. iOS and Android Apps import the real XCFramework/Swift Package and AAR provider binaries; the Android runner verifies byte identity of the AAR and installed APK native library. Physical-device launchers use saved board STA configuration and explicit public fixture URL/STUN build inputs, retain the H2Loader image and replay an immutable per-boot ledger.

Run host qualification:

```sh
bazel test //projects/e2e/apps/pal-webrtc/app:interface_coverage_test //projects/e2e/apps/pal-webrtc/app:reject_missing_test //projects/e2e/targets/cc_binary/pal-webrtc:desktop_test //projects/e2e/targets/pkg_tar/pal-webrtc:browser_test
```

Use the exact manual mobile test targets after selecting dedicated simulators:

```sh
H2_IOS_SIMULATOR_UDID=<test-udid> make bazel-test-ios_pal_webrtc_simulator_test
H2_ANDROID_SERIAL=emulator-5580 H2_WEBRTC_FIXTURE_IP=<host-lan-ip> make bazel-test-android_pal_webrtc_simulator_test
```

Device artifacts live under `projects/e2e/targets/h2loader_tar_zlib/pal-webrtc/{devkit,bk7258_v3_202405}`. Configure `--define=H2_PAL_WEBRTC_OFFER_URL=http://<fixture-ip>:<port>/offer` and `--define=H2_PAL_WEBRTC_STUN_URL=stun:<fixture-ip>:<port>`. Empty endpoints deliberately produce a setup failure. The operator launches an isolated `tools/webrtc-test-server` instance on a host reachable from the board. Do not publish LAN fixture builds as production packages.

A platform is qualified only by a full ledger and matching artifact provenance. Building an artifact does not qualify it. See [qualification.json](qualification.json) for the current six-platform result and per-artifact receipts.

The explicit stream-ID case uses the additive `negotiated=1` configuration: the application coordinates the same ID on both peers before sending. Native H2Peer skips DCEP in this mode, and the browser uses `RTCDataChannelInit.negotiated=true`. Default zero preserves in-band behavior. Native in-band fixed IDs remain supported; browsers explicitly reject that combination because the [WebRTC specification](https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-createdatachannel) ignores `id` in in-band mode. Consumers must rebuild against the matching SDK headers and binaries after the public configuration/metadata structs gain this field.

The wrong-fingerprint case requires valid SDP and a specific authentication failure. Native providers report `TLS_VERIFY`. When Chromium omits RTCError details, the same Pion session independently witnesses a typed received fatal certificate alert (42 or 46) with zero opened channels. Generic handshake errors, disconnected peers, malformed SDP, timeouts and matching error strings cannot pass the gate. Real Pion negative regressions cover invalid signaling and a vanished peer.

## Qualification state

Current macOS, real pinned Chromium, iOS Simulator, Android Emulator, DevKit and BK7258 artifacts each pass all 43 cases, with no failed/blocked cases or retained allocations. Both boards pass after managed installation and a normal App reboot with distinct run IDs, confirmed Apps, empty Stage and preserved P1 Loader. DevKit remains coredump-blank; BK retains its original 32-byte coredump unchanged.

BK r17 additionally completes a separate 600-second active DataChannel/Opus connection on each boot. The first run reports 587 data echoes, 586 unique Opus echoes and one missing packet, with a maximum media gap of 2632 ms. The second reports 587 data echoes, 585 unique Opus echoes and two missing packets, with a maximum gap of 2133 ms. Sequence-tagged packets, Pion RX/TX counters and disabled fixture fault controls preserve end-to-end evidence. Each run requires at least 500 data and media successes; 10 seconds without media recovery or a data/payload failure fails qualification. Real Pion regressions prove recovery after one dropped RTP sequence and rejection of persistent media loss. UART monitor reconnects recover the same immutable boot ledger and do not count as extra runs.

The earlier r16 timeout is retained as [failed-r16.json](../../targets/h2loader_tar_zlib/pal-webrtc/bk7258_v3_202405/evidence/failed-r16.json). Its single-packet-wait probe could not distinguish a lost RTP packet from a stopped media flow. The continuous probe records missing/duplicate packets and bounds the longest media gap while continuing to send. These observations found no additional AP boot during either active window; they do not establish the source of the reported sound or exclude a CP-only reset. No board audio device is opened by this App.

BK uses its existing PSRAM allocator for the conformance peer arena and H2Peer task stacks. Its Net provider implements zero-timeout UDP reads with MSG_DONTWAIT and maps temporary UDP memory pressure to WOULD_BLOCK. DTLS installs SDK cookies, verifies the signaled certificate fingerprint before the minimal SDK profile releases certificate bytes, and rejects missing/mismatched certificates. The SDK's standard fixed-base ECP optimization is enabled through its user-config hook. BK has a bounded 15-second DTLS handshake budget and a 60-second connection test watchdog; host/DevKit retain the 20-second connection default.

Actual SDK host regressions also pass with peer-certificate retention disabled, including cookie retransmission, bilateral records, backpressure, wrong fingerprints on both sides and an omitted client certificate under AddressSanitizer and UndefinedBehaviorSanitizer. These supplement the real Pion board runs. Web media tests deterministically overlap Track unset with both TX and RX main-thread bridges, ensuring no callback borrows the detached Track afterward.
