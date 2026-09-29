# PAL Net/TLS E2E

Independent `h2_pal_net.h` qualification App; existing mixed PAL/HTTP/WebRTC Apps remain separate.

## Contract

The registry has 39 cases. Every one of the 37 mandatory **Net/TLS core profile** cases must pass; missing callbacks, blocked cases, partial bytes, failed fixture verification, timeouts, or retained App ownership prevent qualification. Both optional cases are reported explicitly: an omitted provider callback is `UNSUPPORTED`; a present capability outside the selected profile is `NOT_ASSESSED`. `api_coverage.json` maps all 21 vtable operations to their cases.

The `dns-hostname` case also resolves the existing project endpoint `ap.e2e.gizclaw.com` through real sync/async providers, requires the A record observed by the launcher immediately before the run, overwrites borrowed hostname storage, and requires NOT_FOUND for a session-scoped `.invalid` name. It performs no service request to that endpoint. A DNS record change is an evidence-input update and must be re-observed; numerical fixture resolution alone does not establish hostname resolution.

The core exercises asynchronous resolver lifetime/cancellation/capacity and copied host input, raw UDP/TCP, source address bind, accept/listen recovery, finite I/O budgets, repeated connect progress, exact bidirectional 4097-byte streams, and raw TLS. Required/default custom trust, wrong CA/name, expired certificate, explicit insecure test-only mode, SNI/ALPN, borrowed configuration, handshake timeout, failed-handshake recovery and repeated sessions use isolated controlled peers. This does not infer TLS support from an HTTPS request.

Each TLS rejection requires `H2_PAL_ERR_TLS_VERIFY` and the exact armed peer's observed ClientHello, emitted Certificate, completed failed handshake, and absence of application payload. A refused/malformed endpoint or generic IO error fails. ALPN/SNI are observed at that TLS peer because the PAL does not expose negotiated ALPN. TLS is constrained to TLS 1.2 in the fixture for consistent controlled evidence, not claimed as a full cipher/version interoperability matrix.

`DEFAULT` is tested with an explicit custom CA and must reject an independently generated wrong root. Operating-system trust stores and public roots are **not qualified**. Optional multicast verifies membership setup; multicast delivery, optional ICMP, IPv6, NETIF binding, and DTLS are separate capability scopes. Passing core does not mean every `Net` feature is available. `close`/`resolve_close` return void: the portable ownership ledger records issued release operations; repeated session/capacity tests and launcher teardown supply additional evidence, not process-wide OS leak attribution.

## Platforms

macOS uses native POSIX sockets and WolfSSL. iOS Simulator and Android Emulator import the actual packaged XCFramework/Swift Package and AAR, then create the public native Net owner from `h2_ios_net.h` / `h2_android_net.h`. The owner holds a shared TLS reference; consumers close all handles and quiesce callers before destroying it. Default mobile AppHost assembly stays unchanged and the consumer injects the owner's borrowed API. Simulator results do not represent physical phones.

DevKit and BK7258 consume the Board Net provider with mbedTLS and borrow previously saved Wi-Fi settings. They never store/log a Wi-Fi password. The build embeds only run-local public fixture addresses, public CA certificates, and the public session identifier. A finite setup budget fails without confirming the App. Successful confirmation requires every mandatory case and App cleanup. Qualification requires managed install and an independent normal App boot, with original Loader/P1 retained, empty Stage and unchanged actual coredump bytes.

Browser AppHost supplies canonical unsupported raw Net: standard Fetch/WebRTC does not expose raw TCP/UDP or TLS wrapping. A real C pthread Worker invokes all 21 current unsupported operations and records this capability boundary plus AppHost teardown. It never emits raw TLS PASS or a core qualification. Assessment completion therefore means five supported-platform core receipts plus one explicitly unsupported WASM receipt.

## Commands

Deterministic App/fault and fixture checks (no real PAL-provider E2E):

```sh
bazel test --lockfile_mode=off \
  //projects/e2e/apps/pal-net-tls/app:interface_coverage_test \
  //projects/e2e/apps/pal-net-tls/app:rejection_test \
  //projects/e2e/libs/pal-net-tls-fixture:fixture_evidence_test
```

Manual integration targets have exact corresponding Make entries:

```sh
make bazel-test-desktop_pal_net_tls_test
make bazel-test-desktop_pal_net_tls_endpoint_rejection_test
make bazel-test-wasm_pal_net_tls_boundary_test
H2_IOS_SIMULATOR_UDID=<booted-uuid> make bazel-test-ios_pal_net_tls_simulator_test
H2_ANDROID_SERIAL=<emulator-serial> make bazel-test-android_pal_net_tls_simulator_test
```

To expose a fixture to device Wi-Fi, explicitly choose the local bind/advertised LAN address with `//projects/e2e/libs/pal-net-tls-fixture:serve -- --bind 0.0.0.0 --advertise <LAN-IPv4> --output <temporary-dir>`. Generated `config.json` contains public build inputs named `H2_PAL_NET_TLS_HOST`, `PORT`, `SESSION`, `CA_HEX`, `WRONG_CA_HEX`, `DNS_HOST`, `DNS_IPV4`, and `EPOCH_MS`. Device targets are `//projects/e2e/targets/h2loader_tar_zlib/pal-net-tls/{devkit,bk7258_v3_202405}:package`. Fresh independent peer arm ports prevent a stale rejection receipt from satisfying a later run.

This work's integration order is Wi-Fi qualification first, then Net/TLS. The runner author must verify the Wi-Fi final source/artifact receipts, cleanup/configuration restoration, and hardware release before starting these integration commands. Builds and deterministic fixture/App self-checks may run while Wi-Fi qualification proceeds.

## Evidence

`qualification.json` begins pending; construction is not execution. Receipt verification must compare the ordered registry, source and actual artifact hashes, typed errors and exact current-peer evidence. Board markers replay an immutable boot ledger and do not execute again. Replaying a previous boot does not satisfy independent boot qualification.
