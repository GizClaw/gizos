# PAL WebRTC E2E

This portable qualification App consumes the public WebRTC PAL and a launcher-owned Pion fixture. It is separate from the mixed PAL App and the WebRTC throughput benchmark. The production header, independent inventory and stable case registry define coverage: all 13 peer/channel operations, the read/write Track callbacks, owned event release, custom allocator lifecycle and every DataChannel configuration field.

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

A platform is qualified only by a full ledger and matching artifact provenance. Building an artifact does not qualify it. The browser's native in-band DataChannel creation does not preserve an explicitly requested stream ID under the [WebRTC specification](https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-createdatachannel); that mismatch is currently reported by the gate, not relabeled as success. Six-platform qualification is still in progress.

## Qualification state

The current macOS source passes 43/43 against Pion. BK7258 r15 passes 43/43 after managed installation and again after a normal reboot, with distinct run IDs, no failed/blocked cases or retained allocations, App confirmation, empty Stage, the original P1 and the original 32-byte coredump unchanged. Both UART monitor sessions needed a same-boot reconnect to collect the immutable ledger; reconnects are not extra test executions.

BK uses its existing PSRAM allocator for the conformance peer arena and H2Peer task stacks. Its Net provider now implements zero-timeout UDP reads with MSG_DONTWAIT and treats temporary UDP memory pressure as WOULD_BLOCK. DTLS installs real SDK cookies, verifies the signaled certificate fingerprint before the minimal SDK profile releases certificate bytes, and retains rejection of missing/mismatched certificates. The SDK's standard fixed-base ECP optimization is enabled through its user-config hook. BK has a bounded 15-second DTLS handshake budget and an explicit 60-second connection test watchdog; host/DevKit keep the 20-second test default. Polling and data/media delivery deadlines are unchanged.

The actual SDK host regression also passes with peer-certificate retention disabled, including cookie retransmission, bilateral records, backpressure, wrong fingerprints on both sides and an omitted client certificate, under AddressSanitizer and UndefinedBehaviorSanitizer. It supplements the real Pion board runs; it does not replace them.

Earlier iOS Simulator, Android Emulator and DevKit artifacts have complete 43/43 ledgers; the DevKit artifact also passed a normal reboot. Those receipts must be refreshed after the configurable-watchdog and shared H2Peer changes. Earlier real Chromium records 42 PASS, one explicit-stream-id FAIL, zero BLOCKED and zero retained allocations. This browser limitation remains a mandatory failure; the public stream-ID contract has not been relaxed.

The portable fingerprint-rejected case currently accepts a generic terminal failure. The BK SDK regression separately requires TLS_VERIFY for wrong and missing certificates. Preserving or independently checking that precise reason in the cross-platform gate remains a follow-up. The aggregate stays unqualified and records current-source matching explicitly.
