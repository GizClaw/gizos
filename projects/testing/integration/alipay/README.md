# Alipay integration

This suite composes the owned [Alipay client](../../../../libs/clients/alipay/README.md) into one portable binding/payment App and H106 firmware entries. The client provides the compatible C ABI without an external vendor header tree or binary archive. Native NFC payment qualification is still incomplete; a completed physical write or host test does not establish merchant acceptance or a debit.

## Ownership

[The App API](app/include/h2_alipay.h) owns binding contracts, and [the payment contract](app/include/h2_alipay_payment.h) covers payer codes and NFC wallet sessions. [The concrete adapter](app/include/h2_alipay_e2e_independent.h) translates those operations to the compatible C ABI, caches initialization and owns the context-free SDK callback bridge. It preserves separate SDK, PAL and transport results and prevents repeated physical writes within a session. Runtime owns button/NFC discovery; App workers perform business calls and UID-guarded tag reads/writes. Final entries own physical mappings, provider selection, identity, startup and task policy.

All portable integration headers, implementation and source tests live directly under `app/include`, `app/src` and `app/tests`; offline host tools remain under `tools`. The App BUILD keeps its contracts, UI/workflow, SDK adapter, NDEF encoder and Trace as separate dependency targets within that one source owner. UI/workflow depends on the App contracts and injected operations; the final entry selects the concrete SDK adapter and reply encoder. Existing public symbols and logical archive names are retained.

The [bounded Trace](app/include/h2_alipay_trace.h) owns hashing, capture limits, sequence numbers and explicit drop/truncation statistics. It performs no IO and has one serialized producer; drain/reset only after that worker joins. It records the current adapter's operations without vendor-log, clock or allocator sampling classes. Final-entry and host collector code own transport and evidence serialization.

The current App supports displayed payer QR and screen-on payment inside the Alipay wallet. BLE compatibility remains in the client library and immutable SDK replay fixtures. This integration has no payment BLE scanning, advertising, GATT service or unused alternative reader context. H2Loader management retains its own transport ownership.

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

Entering the wallet's tap page registers `send_data_handle` and `notify_payment_result_handle` before receiving tag data. Runtime publishes discovery state. A business worker reads the discovered tag through PAL with the exact UID, then passes the complete NDEF bytes and exact length unchanged to `alipay_nfc_recv_data_handle`. That entry uses `NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET` (`0`); `AWAKE` (`1`) means an ordinary lit screen outside Alipay. The App does not extract a URI, remove a record header, truncate at a zero byte or regenerate a payment response.

The SDK's send callback supplies application reply bytes. The callback encodes those bytes once and completes one UID-guarded physical PAL write before returning success. It adds no post-write NDEF read or equality check. A failed or ambiguous physical write is never repeated implicitly, and no payment BLE fallback exists in this App.

The ESP provider preserves the selected card from a successful read for one immediately following write to the same UID, avoiding an extra HALT/reselection between request acquisition and reply. Any intervening scan or read, any write attempt, reopening and deinitialization invalidate this selection state. Without a matching read selection, writing follows the normal activation and UID checks. `H2_NFC_WRITE_SELECT mode=read-session` identifies reuse; tag extent, permission and write-response checks still run.

The selected [reply encoder](app/include/h2_alipay_nfc.h) wraps printable ASCII reply bytes in one UTF-8 NDEF Text record using language `en`, bounded to 512 bytes including record headers. On November 18, 2026, the user reported Alipay's confirmation that NFC replies must use Text-type NDEF. The existing record type matches that requirement and is retained. Qualify the complete encoded message, payload offsets/lengths and actual terminal acceptance on the exact firmware; record-type confirmation alone does not establish a successful physical submission or payment. Type 2 page access, TLV/lock checks and publish-last length handling belong to the driver/provider. Unsupported tag types fail before page writes.

Transient tag reads and required pre-write activation use bounded retries. BUSY waits do not consume the three-read budget. The reply worker encodes once and waits at most 100 times for 20 ms when the public write call returns BUSY before device access; it keeps the same UID, message and SDK invocation. Any non-BUSY result ends submission, including I/O and timeout errors that may follow partial physical writes. UID mismatch and unsupported types stop immediately. A real write failure stops the session without querying payment or resending. A completed physical response remains pending until `alipay_query_last_nfc_pay_result` reports the current attempt's actual result; SDK processing callbacks and transport submission do not establish payment.

### Document comparison

Page references use the printed pages in *支付宝智能穿戴 - 碰一下支付（含熄屏碰）*. The current six-field `alipay_nfc_offline_pay.h` takes precedence over the PDF's older four-field callback structure and success-page example.

| PDF pages | Documented behavior | Current integration |
| --- | --- | --- |
| 12–13 | NFC send callback writes the supplied data; BLE broadcast/notify callbacks apply to screen-off payment. | Register NFC send/status callbacks; leave payment BLE callbacks unset. |
| 13–14 | Wallet context is `0`; ordinary awake context outside Alipay is `1`; the context-aware entry supports other watch states. | Use the wallet receive entry and pass the complete NDEF and length unchanged. |
| 15, 17 | Enter the tap page, register callbacks, receive reader data through the SDK and handle callback status. | Start the wallet session before reading; one SDK receive produces one encoded physical reply. |
| 16 | NFC write implementation is a TODO; the example suggests a success page from callback status. | Alipay separately confirmed Text-type replies on November 18, 2026. Physical submission remains subject to device qualification. The current header defines notify status as internal SDK processing; only a real result query can produce a paid receipt. |
| 16–17 | The no-network passive-tag example optionally enters online scan/confirm payment. | Preserve the failure status; this wallet response flow adds no scan/confirm fallback. |
| 17–20 | Screen-off interaction, BLE broadcast and GATT guidance follow the wallet example. | Outside the current screen-on wallet scope. |
| 20–21 | Use a blank NFC card and an Android tag reader to inspect the information read from the merchant terminal. The screenshot shows that incoming URI record. | This is an optional format-debugging procedure, not a required post-payment read or an outgoing reply specification. |

Portable App diagnostics use the injected Log PAL. Native provider/entry logs report phases, errors, lengths and non-payload metadata. The bounded Trace records actual adapter operations; dropped events are explicit. The offline [collector](tools/README.md) validates source, order and capture completeness without opening a device or contacting a service.

`H2_ALIPAY_NFC_WRITE` reports the first BUSY wait and the final PAL result, SDK reply length, encoded NDEF length and wait count at WARN level. These observations distinguish entry contention from provider activation and physical page writes. They contain no payload or UID. The BSP still serializes each scan/read/write using its existing nonblocking flag; the App does not access that private flag or change the public NFC capability.

`H2_NFC_WRITE_REPLY` reports the final page-write response bit count, a decoded ACK/NAK, CRC validation, command/restore results and a failure-only register snapshot sampled after cleanup. The driver accepts a four-bit ACK or the exact ACK byte followed by valid retained CRC_A, using its existing Type A CRC checker. The latter matches the observed terminal control frame `0a a4 fe`; it does not accept unprotected full bytes, NAK, bad CRC or partial frames. `H2_NFC_WRITE_FRAME` adds at most three control-response bytes, a snapshot before FIFO consumption/command cleanup and the pre-command TX/RX/framing configuration. `H2_NFC_READ_FRAMES` also distinguishes pre-cleanup RX state from post-cleanup status, and failed selection reports its command/phase. Interpret each snapshot only when its corresponding status is zero. Neither diagnostic includes the written message, a card UID or a payment token. Permission checks, command ordering and failed-write non-retry behavior remain unchanged. A page ACK alone does not establish complete physical submission or merchant payment acceptance.

`H2_ALIPAY_NFC_INPUT` identifies the complete NDEF passed to the SDK by length and SHA-256. `H2_NFC_WRITE_FINGERPRINT` identifies the existing message in the writer's required pre-write tag read and the expected replacement. These fingerprints reuse `libs/crypto/sha256`, contain no plaintext and add no NFC operation. The App reports its saved input digest after the synchronous SDK call, so SDK input and writer preimage can be compared without inserting diagnostic I/O between acquisition and reply. Different input/preimage fingerprints expose a changed request before writing; an expected fingerprint records intended bytes, not merchant receipt.

## Validation limits

The H106 final entry supplies `https://iotapi.alipay.com/mgw.htm` through `h2_alipay_client_config_t.gateway_url` in [h2_alipay_tiga_protocol.c](../../targets/h2_loader_tar_zlib/integration/alipay/haivivi/h106_tiga_esp_v4_2/main/h2_alipay_tiga_protocol.c). Applications may inject a different complete HTTPS endpoint through the same configuration contract. This is runtime configuration selected by final assembly; the portable SDK does not read target macros or choose a domain.

Host tests cover real owned-client calls, initialization reuse, output guards, cancellation, earlier displayed attempts, Runtime buttons, QR pixels, complete binary NDEF forwarding, NFC acquisition budgets, one encoding/write after BUSY waits, failed-write non-retry, zero payment queries after write failure and resource cleanup. Adapter and UI tests separate SDK processing success, pending response submission and actual SDK result queries, including query failure and a zero-amount paid result. The seven immutable client fixtures preserve their original hashes and 10,416 archived records. Current replay compares 10,378 supported records, explicitly excluding 38 downloaded-script records and 23 additional image-decode comparisons. Image decoding and downloaded transit scripts remain unsupported.

Native compilation, flashing, merchant interoperability, issuer/account eligibility and financial results require separate evidence for the exact firmware. NFC payment has not passed real-device validation. See [the payment test plan](payment_test_plan.md); host tests and completed SPI/DMA or NFC writes do not close its hardware acceptance criteria.
