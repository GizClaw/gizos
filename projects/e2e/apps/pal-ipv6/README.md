# PAL IPv6 E2E

`pal-ipv6` is an independent IPv6 suite. It runs the existing 37 mandatory Net/TLS cases with IPv6 peers, then 19 address-selection and application cases. The native gate requires all 56 cases, exact peer/session payloads and successful cleanup. A missing network is BLOCKED, a missing provider is UNSUPPORTED, and an unexpected result is FAIL. Each new receipt owns its source/artifact/session identity; previous Net/TLS and GizClaw receipts retain their historical identities.

Native scope includes IPv6 literals, family-filtered and asynchronous DNS, copy/cancel/capacity, scoped link-local addresses, V6-only socket ownership, UDP source/receive/truncation/deadlines, TCP listen/accept/recovery and partial streams, strict TLS trust/name/SNI/ALPN/rejection/deadlines, interface binding, HTTP on a bracketed IPv6 literal, dual-stack HTTP fallback, MQTT publish, DTLS payload/fingerprint rejection/deadline over real IPv6 UDP, and the complete 43-case WebRTC/SCTP suite against Pion configured with IPv6 ICE candidates only.

The POSIX DNS list preserves the order of retained resolver answers, deduplicates addresses, and reports truncation beyond eight answers while reserving an answer of each family. Both providers resolve the reserved localhost name to actual loopback addresses; this remains valid when the OS hosts file omits its IPv6 entry. A separate nonce-bound authoritative DNS fixture exchanges real AAAA packets over IPv6 UDP through `libs/dns`; it does not reconfigure the system resolver. ESP queries both families because lwIP's AF_UNSPEC resolver can return only one answer. Legacy single-address calls prefer IPv4 but accept an AAAA-only host. New list operations retain the family filter. Callers open a separate socket for each family; failed connection attempts close before fallback and consume the original deadline. IPv6 scope IDs are host-local interface indices and are not encoded in SDP or STUN.

WASM exercises actual browser Fetch on an IPv6 literal and WebRTC/DTLS/SCTP on an IPv6 ICE link. Its canonical raw Net boundary remains an explicit SKIP; a browser PASS never claims raw TCP/UDP/TLS or raw resolver qualification. Signaling forwarding in the browser harness carries only fixture SDP/control; ICE/data/media traffic goes directly to the local IPv6 Pion endpoint.

## Commands

The Desktop test starts all declared peers on loopback and runs automatically in the compatible host CI graph without tags. Browser and mobile tests require their prepared environment and remain `manual`/`external`, so each invocation observes fresh state while build/disk caches remain on. Explicit external DNS qualification with the Desktop target must use `--nocache_test_results`.

```sh
bazel test --config=macos_arm64 \
  //projects/e2e/targets/cc_binary/pal-ipv6:desktop_pal_ipv6_test \
  //projects/e2e/targets/pkg_tar/pal-ipv6:wasm_pal_ipv6_test
bazel test --config=ios_sim_arm64 \
  '--test_env=H2_IOS_SIMULATOR_UDID=<booted-uuid>' \
  //projects/e2e/targets/ios_application/pal-ipv6:ios_pal_ipv6_simulator_test
bazel test --config=android_arm64 \
  '--test_env=H2_ANDROID_SERIAL=<emulator-serial>' \
  '--test_env=H2_PAL_IPV6_ANDROID_HOST=<reachable-host-IPv6>' \
  '--test_env=H2_PAL_IPV6_BIND=<local-IPv6-bind-address>' \
  --test_env=ANDROID_HOME --test_env=ANDROID_NDK_HOME \
  //projects/e2e/targets/android_binary/pal-ipv6:android_pal_ipv6_simulator_test
bazel build --config=esp32s3 \
  //projects/e2e/targets/h2loader_tar_zlib/pal-ipv6/devkit:package
```

The Android peer must be reachable using actual IPv6. The emulator defaults to its IPv6 host alias `fec0::2`, after independent TCP/UDP reachability checks. `10.0.2.2` is not an IPv6 substitute. Simulator/emulator receipts are distinct from physical phone qualification. `H2_PAL_IPV6_DNS_HOST` can choose an independently observed AAAA hostname; the default host/mobile fixture uses the system localhost resolver. The dual-stack DNS case requires actual A and AAAA answers in the resolver view.

DevKit enables lwIP IPv6/autoconfiguration/loopback in its own E2E configuration. It borrows saved Wi-Fi settings, creates link-local addressing and uses explicitly provided public fixture inputs prefixed `H2_PAL_IPV6_`. The host-side interface index is not the device's scope index: scoped device destinations must use the DUT's actual Netif index. No password is embedded or written. Directed port/UID, IPv6 LAN reachability, source/image/package hashes, a fresh boot/peer ledger, preserved Loader/P1, empty Stage, unchanged coredump and successful confirmation are required for a hardware receipt. Replayed case markers do not rerun tests. The fixture does not infer device readiness from a package build.

The first acceptance matrix is macOS, WASM, iOS Simulator, Android Emulator and DevKit. BK7258 and Windows qualification are deferred. IPv6-only external routing, RA/default-route changes and physical interface removal need a separately supplied network; localhost execution alone does not qualify those conditions.

Android listener cases use the existing adb control bridge to connect to the DUT's actual IPv6 loopback listener. The accepted address must still be IPv6; this does not qualify routed inbound IPv6. Client TCP/UDP/TLS, HTTP, MQTT and ICE traffic use the real IPv6 emulator host path. The system-DNS localhost expectation is independently collected from the DUT loopback interface. External AAAA resolver views, IPv6-only WAN access and network changes remain separate environment qualification; IPv4-mapped DNS answers cannot prove them.

Keep the physical-board peers alive with `bazel run --config=macos_arm64 //projects/e2e/libs/pal-ipv6-fixture:serve -- --bind :: --advertise <host-ULA-or-global-IPv6> --output <temporary-dir>`. This generates only public fixture/trust inputs in `config.json` and `fixture.bazelrc`, plus an immutable-session peer ledger. Build with `bazel --bazelrc=<temporary-dir>/fixture.bazelrc build --config=esp32s3 //projects/e2e/targets/h2loader_tar_zlib/pal-ipv6/devkit:package` while those peers remain alive. The full ICE matrix requires ULA/global addressing; scoped raw link-local handling is a separate socket contract. Reserved localhost resolution follows [RFC 6761](https://www.rfc-editor.org/rfc/rfc6761.html#section-6.3).

## Two-board IPv6 bench

Reserved `.invalid` names return `NOT_FOUND` locally, including subdomains, case variants and a trailing dot. The rejection is independent of the AP's DNS availability; it follows [RFC 6761](https://www.rfc-editor.org/rfc/rfc6761.html#section-6.4) and is checked separately from the real authoritative AAAA exchange.

The AMOLED fixture provides the Wi-Fi AP and all remote test services. DevKit connects directly to it with the non-saving PAL STA operation. The Mac only builds firmware, installs through H2Loader and collects serial logs. It does not join the AP or relay network payloads. The fixture advertises `fd53:697a:6f73:626::/64` with SLAAC and serves at `fd53:697a:6f73:626::1`; DevKit must acquire an address in that prefix before testing. The router lifetime is zero because this bench provides on-link traffic, with no upstream default route. Prefix advertisements follow the [RFC 4861 message format](https://www.rfc-editor.org/rfc/rfc4861.html#section-4.2).

The same 56-case DevKit registry runs against a board-owned raw TCP/UDP/TLS control protocol, strict server-side handshake/payload proofs, IPv6 HTTP/MQTT/DNS and STUN. The 43-case WebRTC registry uses the existing portable H2Peer answer engine on AMOLED, including encrypted DataChannel and Opus echoes and the actual selected IPv6 pair. These board results qualify H2Peer-to-H2Peer behavior; the host/mobile Pion interoperability receipts retain their separate provenance. Direct DTLS endpoint cases and localhost resolver/fallback checks still run locally on DevKit, while the raw peer, HTTP, MQTT, authoritative AAAA query, ICE, SCTP and media use the wireless board-to-board path.

Generate one fresh session with `python3 projects/e2e/libs/pal-ipv6-board-fixture/configure.py --output <new-temporary-directory>`. It writes a public `fixture.bazelrc` for DevKit and a private `server.bazelrc` containing ephemeral test-only TLS keys for AMOLED. Build the AMOLED `//projects/e2e/targets/h2loader_tar_zlib/pal-ipv6/amoled-fixture:package` using `server.bazelrc`, and the existing DevKit package using `fixture.bazelrc`. Neither image contains the saved upstream Wi-Fi password. The temporary AP uses the declared bench credential, accepts one station, and does not change saved STA settings.

Before installation, bind both live ports to their exact UIDs and preserve full original packages, Loader/P1 metadata, Stage and coredump status. Require the AMOLED fixture's fresh boot/session/ready/confirmation markers, the DevKit's SLAAC address, all case receipts, server-side proofs, zero retained DUT resources, and matching package/image identities. Preserve hardware test status separately from any subsequent restoration of the original firmware and Stage. Firmware builds alone do not qualify this bench.

The board fixture unwinds startup and confirmation failures: it stops and joins every started service before closing its listeners, SCTP/peer state and mutex. The AMOLED launcher then stops its owned AP, cancels the RA timeout and removes the raw advertiser on the TCP/IP thread. Late queued startup/timeout callbacks cannot restart an advertiser after stop. A terminal PAL join/destroy failure retains ownership and borrowed dependencies instead of freeing a live context; the failure receipt must identify that cleanup error. Host failure-injection tests cover these paths, and a later native/physical receipt keeps its own source and image identity.

Desktop ICE composition explicitly selects the real loopback interface and its owned Net resolver localhost address. An active NIC or a loopback interface may also expose scoped link-local IPv6; that address cannot substitute for the fixture's unscoped local peer. The public provider selection policy is unchanged, every socket/wire exchange remains real, and the runner requires independently observed IPv6 selected pairs.
