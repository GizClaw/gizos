# PAL Net/TLS E2E

Independent `h2_pal_net.h` qualification App; existing mixed PAL/HTTP/WebRTC Apps remain separate.

## Contract

The registry has 39 cases. On a provider implementing the raw **Net/TLS core profile**, all 37 mandatory cases must pass; missing callbacks, blocked cases, partial bytes, failed fixture verification, timeouts, or retained App ownership prevent core qualification. The six-platform assessment uses the user-selected capability policy: exercise available capabilities and record genuinely unavailable capabilities as `SKIP`, with the observed `UNSUPPORTED` result and reason. A skipped capability never counts as functional PASS or core qualification. Optional multicast membership and ICMP are exercised wherever their callbacks exist; an omitted callback is recorded as `UNSUPPORTED` and assessed as a skip. No supported optional case may remain `NOT_ASSESSED`. `api_coverage.json` maps all 21 vtable operations to their cases.

The `dns-hostname` case resolves the existing project endpoint `ap.e2e.gizclaw.com` through real sync/async providers. It records launcher and device A records separately because geographic DNS views may differ. Each result must be a usable IPv4 address; the device sync/async results must agree after the borrowed hostname buffer is overwritten. A session-scoped `.invalid` name must return NOT_FOUND. No service request is sent to that endpoint.

The core exercises asynchronous resolver lifetime/cancellation/capacity and copied host input, raw UDP/TCP, source address bind, accept/listen recovery, finite I/O budgets, repeated connect progress, exact bidirectional 4097-byte streams, and raw TLS. Required/default custom trust, wrong CA/name, expired certificate, explicit insecure test-only mode, SNI/ALPN, borrowed configuration, handshake timeout, failed-handshake recovery and repeated sessions use isolated controlled peers. This does not infer TLS support from an HTTPS request.

Each TLS rejection requires `H2_PAL_ERR_TLS_VERIFY` and the exact armed peer's observed ClientHello, emitted Certificate, absence of application payload. A provider may detect expiry after the server-side handshake; then the peer must observe a zero-payload close. A refused/malformed endpoint or generic IO error fails. ALPN/SNI are observed at that TLS peer because the PAL does not expose negotiated ALPN. TLS is constrained to TLS 1.2 in the fixture for consistent controlled evidence, not claimed as a full cipher/version interoperability matrix.

`DEFAULT` is tested with an explicit custom CA and must reject an independently generated wrong root. Operating-system trust stores and public roots are **not qualified**. Optional multicast verifies membership setup and optional ICMP verifies an actual echo response. Multicast delivery, IPv6, NETIF binding, and DTLS remain separate capability scopes. Passing core does not mean every `Net` feature is available. `close`/`resolve_close` return void: the portable ownership ledger records issued release operations; repeated session/capacity tests and launcher teardown supply additional evidence, not process-wide OS leak attribution.

## Platforms

macOS uses native POSIX sockets and WolfSSL. iOS Simulator and Android Emulator import the actual packaged XCFramework/Swift Package and AAR, then create the public native Net owner from `h2_ios_net.h` / `h2_android_net.h`. The owner holds a shared TLS reference; consumers close all handles and quiesce callers before destroying it. Default mobile AppHost assembly stays unchanged and the consumer injects the owner's borrowed API. Simulator results do not represent physical phones.

DevKit and BK7258 consume the Board Net provider with mbedTLS and borrow previously saved Wi-Fi settings. The App does not print or package a Wi-Fi password; lower-level boot logs may contain saved configuration, so checked-in serial evidence is redacted before its hash is bound to the board receipt. The build embeds only run-local public fixture addresses, public CA certificates, and the public session identifier. A finite setup budget fails without confirming the App. Successful confirmation requires every mandatory case and App cleanup. Qualification requires managed install and an independent normal App boot, with original Loader/P1 retained, empty Stage and unchanged actual coredump bytes.

Browser AppHost currently supplies canonical unsupported raw Net: standard Fetch/WebRTC does not expose raw TCP/UDP or TLS wrapping. A real C pthread Worker invokes all 21 current unsupported operations and records this diagnostic boundary plus AppHost teardown. It never emits raw TLS PASS or a core qualification. The assessment explicitly skips its 37 mandatory and two optional raw Net/TLS cases after validating those actual unsupported results, Worker identity and successful AppHost teardown. Browser-local semantics are retained: no remote-network relay is introduced. Fetch and WebRTC are qualified by their own Apps, not counted as raw Net/TLS support.

## Commands

Deterministic App/fault and fixture checks (no real PAL-provider E2E):

```sh
bazel test --lockfile_mode=off \
  //projects/e2e/apps/pal-net-tls/app:interface_coverage_test \
  //projects/e2e/apps/pal-net-tls/app:rejection_test \
  //projects/e2e/libs/pal-net-tls-fixture:fixture_evidence_test
```

Invoke the exact Bazel integration targets directly; platform configuration belongs to `.bazelrc`, and static suite parameters belong to `BUILD.bazel`. All five live test declarations use Bazel's native `external` tag to disable cached test results while keeping build caching enabled, even when the caller uses the default cache flag. The commands below also make fresh observation explicit. Reserve a booted simulator and replace the device placeholders; Android also needs the configured SDK/NDK paths exported in the caller environment.

```sh
bazel test --config=macos_arm64 --nocache_test_results \
  //projects/e2e/targets/cc_binary/pal-net-tls:desktop_pal_net_tls_test \
  //projects/e2e/targets/cc_binary/pal-net-tls:desktop_pal_net_tls_endpoint_rejection_test
bazel test --config=macos_arm64 --nocache_test_results \
  //projects/e2e/targets/pkg_tar/pal-net-tls:wasm_pal_net_tls_boundary_test
bazel test --config=ios_sim_arm64 --nocache_test_results \
  '--test_env=H2_IOS_SIMULATOR_UDID=<booted-uuid>' \
  //projects/e2e/targets/ios_application/pal-net-tls:ios_pal_net_tls_simulator_test
bazel test --config=android_arm64 --nocache_test_results \
  '--test_env=H2_ANDROID_SERIAL=<emulator-serial>' \
  --test_env=ANDROID_HOME --test_env=ANDROID_NDK_HOME \
  //projects/e2e/targets/android_binary/pal-net-tls:android_pal_net_tls_simulator_test
```

Bazel builds/packages the consumer and calls the shared mobile Python engine directly. Python owns real fixture setup, App installation/launch, peer assertions and cleanup; it does not invoke Bazel. No per-target Make/shell/Python scheduling wrapper is required.

To expose a fixture to device Wi-Fi, explicitly choose the local bind/advertised LAN address with `bazel run --config=macos_arm64 //projects/e2e/libs/pal-net-tls-fixture:serve -- --bind 0.0.0.0 --advertise <LAN-IPv4> --output <temporary-dir>`. Generated `config.json` contains public build inputs named `H2_PAL_NET_TLS_HOST`, `PORT`, `SESSION`, `CA_HEX`, `WRONG_CA_HEX`, `DNS_HOST`, `DNS_IPV4`, and `EPOCH_MS`. Device targets are `//projects/e2e/targets/h2loader_tar_zlib/pal-net-tls/{devkit,bk7258_v3_202405}:package`. Fresh independent peer arm ports prevent a stale rejection receipt from satisfying a later run.

This work's integration order is Wi-Fi qualification first, then Net/TLS. The runner author must verify the Wi-Fi final source/artifact receipts, cleanup/configuration restoration, and hardware release before starting these integration commands. Builds and deterministic fixture/App self-checks may run while Wi-Fi qualification proceeds.

## Evidence

`qualification.json` uses the supported-PASS / unsupported-SKIP policy. Current macOS, iOS Simulator, Android Emulator and BK7258 receipts each pass all 37 mandatory cases through actual providers. BK7258 R9 passes a managed install and independent normal boot of the same package, with distinct boot IDs and exact peer ledgers. Its receipt binds package/image hashes, UID, preserved Loader/P1, empty Stage and three byte-identical coredumps. DevKit R8's two successful core boots remain preserved, but its newly enabled ICMP case requires a fresh R9 run: the USB link stopped responding during the first R9 staging attempt, so DevKit assessment remains `PENDING` until physical reset/reconnection and complete requalification. The failed transfer is not counted as install or qualification success. Browser WASM is explicitly `SKIP`, with all 21 actual unsupported operations verified on its C Worker. Checked-in board serial logs redact saved Wi-Fi settings before their hashes are bound.

The default `check_qualification.py` and Bazel evidence target require the entire six-platform assessment to be complete and no supported case to remain unassessed. `--allow-pending` is diagnostic only while a live run is unavailable; it still verifies every claimed pass and skip. `--require-all-core` additionally rejects the browser capability skip, so assessment completion never implies six raw Net/TLS implementations. The mobile declarations use the shared Python runner; only named optional cases may skip on typed `UNSUPPORTED`, and cleanup failures remain fatal.

Receipt verification compares the ordered registry, source and artifact hashes, typed errors, complete peer payloads, and the exact current-peer run. Board markers replay an immutable boot ledger and do not execute again. Replaying a previous boot does not satisfy independent boot qualification. `projects/e2e/libs/pal-net-tls-device/assemble_board.py` performs the full source-log/status/metadata/coredump assembly and rejects missing or stale fields.

### Browser raw Net/TLS capability boundary

The real webpage probe runs on a C pthread Worker in a cross-origin-isolated page. It checks all 21 actual AppHost Net vtable slots: 19 return `H2_PAL_ERR_UNSUPPORTED`, and the two void release operations are canonical no-ops with no owned handles. Its receipt records the actual archive and browser hashes, execution identity and teardown result. The checker rejects missing/duplicate/unknown slots, generic errors, retained ownership, main-thread execution, false core PASS, or unsuccessful teardown.

The 39 skipped case IDs in the platform assessment are dispositions derived from this unavailable provider, not claims that the portable functional cases executed. The functional registry stays unchanged. A future implementation with raw browser-local capabilities must execute their applicable cases and cannot reuse this blanket skip while returning supported operations. No WSS relay, relay-host addresses or production relay deployment is part of this change.
