# Alipay integration

This suite composes the owned [Alipay client](../../../../libs/clients/alipay/README.md) into one portable binding/payment App and H106 firmware entries. The client provides the compatible C ABI without an external vendor header tree or binary archive. Native NFC payment qualification is still incomplete; a completed write/readback or host test does not establish merchant acceptance or a debit.

## Ownership

[The App API](app/include/h2_alipay.h) owns binding contracts, and [the payment contract](app/include/h2_alipay_payment.h) covers payer codes and NFC wallet sessions. [The concrete adapter](independent/include/h2_alipay_e2e_independent.h) translates those operations to the compatible C ABI, caches initialization and owns the context-free SDK callback bridge. It preserves separate SDK, PAL and transport results and prevents repeated physical writes within a session. [Diagnostics](diagnostics/README.md) owns only the bounded Trace. Runtime owns button/NFC discovery; App workers perform business calls and UID-guarded tag reads/writes. Final entries own physical mappings, provider selection, identity, startup and task policy.

The current App supports displayed payer QR and NFC wallet payment. BLE compatibility remains in the client library and immutable SDK replay fixtures. This integration has no payment BLE scanning, advertising, GATT service or unused alternative reader context. H2Loader management retains its own transport ownership.

## Build and preparation

```sh
bazel test --config=macos_arm64 //libs/clients/alipay:all //projects/testing/integration/alipay/...
bazel build --config=esp32s3 //projects/testing/targets/h2_loader_tar_zlib/integration/alipay/haivivi/h106_tiga_esp_v4_2:package
```

Activate the declared ESP-IDF environment before a native build. Both package formats consume one firmware graph; the zlib_tar entry borrows the tar_zlib entry's raw inputs. The explicit public Wi-Fi fixture is `HAIVIVI-MFG` / `!haivivi`. Preparation establishes IP and calibrated UTC. Actual HTTP request errors are observed through the injected HTTP capability; there is no additional DNS/TCP/TLS/HEAD preflight that can independently block binding.

The H106 entry selects the existing factory SN/IMEI, wallet context and `PAYCODE | NFCPAY | SQUARE` (`0x4005`). Default storage is session-only: bounded blob/U32 values survive namespace close within this boot, while reset discards them. The active volatile namespace is `h106_pay_ctx`; certificates may use the existing persistent certificate namespace. Historical namespaces, including old PSA stores, remain in the explicit cleanup/audit policy without gaining new active writable routes. Factory identity and unrelated data are preserved. An explicit `H2_ALIPAY_E2E_PERSISTENT_STORAGE` build opts into the real Pref provider; host readback never proves cross-reset durability.

## Binding and payer QR

Initialization is serialized and cached within the configured adapter lifetime. The App displays one binding code and polls the same session after display completion. Successful finish, committed/read-back TID, UID, shared key, seed and a matching completion record are required before entering the payment menu. Pending binding has no App deadline; individual transports retain finite budgets. Terminal failure can request a replacement only after an unbound snapshot, without resetting unverified credentials.

Four distinct Runtime Button mappings provide previous, next, activate and back actions on release. The App opens no Touch PAL. Display rendering and LVGL calls remain on the App thread; SDK operations run on one worker at a time. Shutdown retains dependencies until worker join succeeds.

Payer codes contain 18 or 19 digits and use the existing QR encoder with level Q, integer scale and a quiet zone. Refresh is scheduled every 60 seconds, on user refresh and on a material clock step. The model retains a bounded set of previously displayed codes so queries continue to identify the original attempt after refresh. Code generation and display do not establish payment. Only an actual successful result query produces a local paid receipt; errors and missing results remain visible. Codes and packets are not printed or persisted.

## NFC wallet transport

Runtime publishes discovery state. A business worker reads the discovered tag through PAL with the exact UID, then passes complete NDEF bytes to the owned receive entry. The callback encodes application reply bytes and completes the UID-guarded PAL write/readback before returning success. A failed or ambiguous physical write is never repeated implicitly, and no payment BLE fallback exists in this App.

The selected [reply encoder](payment/include/h2_alipay_nfc.h) writes one UTF-8 NDEF Text record using language `en`, bounded to 512 bytes including record headers. This is an explicit, unqualified transport profile: the supplied callback contract does not establish merchant record type/language acceptance. Type 2 page access, TLV/lock checks, publish-last length handling and physical verification belong to the driver/provider. Unsupported tag types fail before page writes.

Transient tag reads and pre-write activation use bounded retries. BUSY waits do not consume the three-read budget. UID mismatch and unsupported types stop immediately. A completed physical response remains pending until the subsequent SDK result query reports success; SDK processing callbacks and transport submission do not establish payment.

Portable App diagnostics use the injected Log PAL. Native provider/entry logs report phases, errors, lengths and non-payload metadata. The bounded Trace records actual adapter operations; dropped events are explicit. The offline [collector](tools/README.md) validates source, order and capture completeness without opening a device or contacting a service.

## Validation limits

Host tests cover real owned-client calls, initialization reuse, output guards, cancellation, earlier displayed attempts, Runtime buttons, QR pixels, NFC acquisition budgets, failed-write non-retry and resource cleanup. The seven immutable client fixtures preserve their original hashes and 10,416 archived records. Current replay compares 10,378 supported records, explicitly excluding 38 downloaded-script records and 23 additional image-decode comparisons. Image decoding and downloaded transit scripts remain unsupported.

Native compilation, flashing, merchant interoperability, issuer/account eligibility and financial results require separate evidence for the exact firmware. NFC payment has not passed real-device validation. See [the payment test plan](payment_test_plan.md); host tests and completed SPI/DMA or tag readback do not close its hardware acceptance criteria.
