# PAL HTTP E2E

`//projects/e2e/apps/pal-http/app:pal_http_e2e` owns a mandatory 45-case registry for both public HTTP operations and all request fields. It borrows Runtime HTTP/Memory/Time APIs and fixture URLs. Concrete providers, CA trust, browser policy, Wi-Fi, packaging and boot lifecycle belong to launchers. The existing mixed PAL App and Atomic App are unchanged.

## Contract

Cases cover seven methods, binary upload/download, bounded non-NUL byte spans, exposed response headers, empty/HEAD and HTTP error responses, caller-buffer bounds, allocator ownership and alias precedence, allocation failure, three streaming forms, discard mode, callback errors, cancellation before/during transfer, cancellation while waiting, response/whole-retry deadlines, bounded retries, relative/303/307 redirects, trusted/untrusted HTTPS, invalid inputs, interface rejection, idempotent release, 25 consecutive allocate/free requests and callback quiescence after success/error. Every response is released; tracked response/scratch allocations must return to zero. Missing request/free operations block all cases and cannot qualify.

The fixed 513-byte fixture includes NUL and non-ASCII bytes. The streaming callback verifies each byte, monotonically increasing total, known/unknown remaining length, caller chunk-buffer identity and capacity. It requires `body_len` to count streamed/discarded bytes. Allocation failure is injected through the actual request allocator. No oversized length is paired with an undersized allocation. Backend transport/DNS/TLS resource cleanup additionally remains covered by provider tests and native integration evidence.

`interface-rejected` verifies that a nonexistent requested interface cannot silently route through another interface. Browser Fetch explicitly returns UNSUPPORTED for interface binding; this case validates that rejection and does not claim browsers can select a physical interface or open raw sockets. HTTPS tests use an isolated test CA; the browser pins only the ephemeral trusted fixture certificate's SPKI and must reject the separate untrusted key. Certificate checking is never globally disabled. This App does not qualify Wi-Fi provisioning, raw Net/TLS sockets, MQTT, real Internet services, proxies, HTTP/2, TLS cipher coverage or arbitrary browser-forbidden headers.

## Fixture and results

`projects/e2e/libs/pal-http-fixture` owns stdlib HTTP/HTTPS servers and creates short-lived certificates with OpenSSL. Every run has an independent random session path and retry counters. Host/browser tests bind loopback, use ephemeral ports, shut down owned processes/servers and do not read production credentials. The browser performs real cross-origin Fetch with CORS and cross-origin isolation enabled.

Each case emits `H2_PAL_HTTP_CASE` JSON. Qualification requires the exact ordered registry, every status PASS, no duplicate/missing rows, zero failed/blocked/retained allocations and successful provider teardown. `H2_PAL_HTTP_SUMMARY` is the final aggregate. Bazel tests write a structured `qualified.json` to undeclared test outputs; artifact and registry SHA-256 bind the receipt to its inputs. Committed receipts describe only the artifacts actually run.

## Launchers

| Platform | Entry | Verification |
| --- | --- | --- |
| macOS / Linux | `targets/cc_binary/pal-http` | Production OS Net/TLS + CoreHTTP; real local HTTP/HTTPS peer |
| WASM | `targets/pkg_tar/pal-http` | C runs in a pthread Worker; real Chromium Fetch; no pending JS requests after teardown |
| iOS simulator | `targets/ios_application/pal-http` | Real Swift Package archive; CoreHTTP + POSIX Net + full WolfSSL; system SecRandom entropy |
| Android emulator | `targets/android_binary/pal-http` | Real AAR binary checked against APK; same full provider; system `/dev/urandom` entropy |
| DevKit ESP32-S3 | `targets/h2loader_tar_zlib/pal-http/devkit` | USB H2Loader command service, saved STA settings, independent CoreHTTP with injected test CA |
| BK7258 AP | `targets/h2loader_tar_zlib/pal-http/bk7258_v3_202405` | UART1 H2Loader command service, saved STA settings, independent CoreHTTP with injected test CA |

```sh
bazel test //projects/e2e/apps/pal-http/app:interface_coverage_test //projects/e2e/apps/pal-http/app:reject_missing_test
bazel test //projects/e2e/targets/cc_binary/pal-http:desktop_test //projects/e2e/targets/pkg_tar/pal-http:browser_test
```

iOS/Android exact manual entries are `make bazel-test-ios_pal_http_simulator_test` and `make bazel-test-android_pal_http_simulator_test`, with an explicit `H2_IOS_SIMULATOR_UDID` or `H2_ANDROID_SERIAL`. Run tests sharing one simulator sequentially (`--local_test_jobs=1` when batching), because only one App can own its foreground scene. A firmware build is not device qualification, and host/browser receipts do not stand in for simulators or boards. The aggregate qualification receipt records each platform's current evidence separately.
