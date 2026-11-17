# Alipay integration test

This suite owns binding/payment interaction, Runtime acquisition, public PAL transport, bounded diagnostics and real-system qualification. All business calls use the [self-implemented Alipay package](../../../../libs/clients/alipay/README.md) and its [owned public declarations](../../../../libs/clients/alipay/include/h2_alipay_api.h). The build graph contains no external Alipay import or native vendor adapter.

[The client adapter](independent/include/h2_alipay_e2e_independent.h) invokes the portable implementation and records non-secret results. [Diagnostics](diagnostics/README.md) defines the App-facing contracts and bounded trace. [The payment bridge](payment/include/h2_alipay_payment_bridge.h) owns callback transport, attempt state and transmission guards. [The App](app/include/h2_alipay_e2e.h) owns user interaction; the final entry owns factory identity, Runtime lifetime, provider selection and task policy.

## Build and storage

The native entry is `//projects/testing/targets/h2_loader_tar_zlib/integration/alipay/haivivi/h106_tiga_esp_v4_2:package`. The corresponding `h2_loader_zlib_tar` entry borrows its raw inputs. Both preserve board `tiga_esp_v4_2`, target `esp32s3`, role `app`, image identity `alipay-e2e-independent` and version `0.1.0-e2e`. There is one native image graph.

```sh
bazel build --config=esp32s3 \
  //projects/testing/targets/h2_loader_tar_zlib/integration/alipay/haivivi/h106_tiga_esp_v4_2:package \
  //projects/testing/targets/h2_loader_zlib_tar/integration/alipay/haivivi/h106_tiga_esp_v4_2:package
```

Activate the declared ESP-IDF environment first. No Alipay-specific external SDK path is required. The explicit public Wi-Fi fixture is `HAIVIVI-MFG` / `!haivivi`. SNTP calibrates UTC before business calls; the gateway probe checks DNS, bounded TCP, verified TLS and a credential-free HEAD request.

Default storage is session-only. [Entry-owned variables](../../targets/h2_loader_tar_zlib/integration/alipay/haivivi/h106_tiga_esp_v4_2/main/h2_alipay_tiga_storage.h) provide bounded blob/U32 Pref values across namespace close within this boot; reset discards them. No generic memory backend is selected. Wrong-type reads fail and failed allocation preserves the prior value.

Before a session starts, the existing authorized cleanup verifies the known historical test namespaces are empty and factory identity is unchanged. Unrelated Pref and certificate namespaces remain intact. After business workers join, a persistent-store audit confirms test material stayed volatile. Session completion is not persistent-storage qualification.

## Binding and identity

Initialization is serialized and its result cached. The App displays one binding code, polls the same session and requires successful finish plus committed/read-back TID, UID, shared key and seed before entering the payment menu. Pending binding has no App deadline; individual transport calls retain finite budgets. Terminal failure can obtain a replacement only after an unbound snapshot, without resetting unverifiable credentials.

`H2_ALIPAY_BINDING_WAIT` marks manual phone authorization. The App does not authorize an account, unbind, provision or submit a payment automatically. LVGL and result presentation remain on the UI thread; business workers return owned results and join before teardown.

Factory SN comes from the existing `h106_factory/imei` key and is used consistently as SN/IMEI. Identity acquisition does not query the modem. Manufacturer `HAIVIVI`, raw model `H106` and child-watch type 3 remain the selected product values. The binding registration model is `QZ.SB.LY.HAIVIVI-H106`. Product getters report those local values, not platform approval.

## Payer code

The payer page renders the generated code as a Q-level QR, refreshes explicitly and at 60 seconds, and hides expired codes. It retains up to three private 120-second attempts and queries their actual displayed values at a shared two-second interval. A refresh cannot substitute a new code into an older query.

Code generation and display do not establish a debit. A validated successful result establishes the page's receipt; query errors show the PAL/API result and pending status remains a wait. Codes and packets are neither printed nor persisted. Host tests decode the rendered framebuffer before and after refresh and complete the original attempt while a newer code is visible. Separate Code128 presentation remains unimplemented.

## NFC response

The H106 entry selects wallet context, `PAYCODE | NFCPAY | SQUARE` (`0x4005`) and `payment_nfc_only`. Runtime owns discovery. One worker reads the current tag through PAL and passes the complete NDEF to the self-implemented receive entry. Its reply callback encodes application bytes and invokes the existing UID-guarded PAL NDEF write/readback.

The selected [encoder](payment/include/h2_alipay_nfc.h) uses a single UTF-8 NDEF Text record with language `en`, bounded to 512 bytes including record headers. This is an explicit transport profile awaiting merchant-terminal qualification: the supplied callback contract does not specify the record type/language. Complete source/message-byte tests and physical write/readback cannot by themselves establish that the merchant accepts this encoding.

Payment BLE scanning, response advertising and GATT remain inactive in this mode. H2Loader management retains its own transport ownership. Before any physical write, transient selection IO/protocol/timeout failures permit at most three activation attempts, with 20 ms between attempts when the transport supplies a sleep callback. A successful selection still requires the original exact UID and a supported Type 2 tag. UID mismatch or unsupported type stops immediately. A failed or ambiguous physical write is never retried or replaced with a BLE payment response. User entry into Tap authorizes this wallet-context response; the physical device must remain close enough to complete the write.

The FM175xx provider supports formatted, unlocked Type 2 NDEF read/write. The writer locates NDEF after NULL, Lock Control and Memory Control TLVs, preserves the prefix and partial-page neighbours, checks declared dynamic lock bits and rejects reserved-page overlap. Layouts that require the TLV stream or message to skip reserved pages remain unsupported. The observed machine selects Type A with final SAK zero and supplies a 292-byte message. Complete reads have succeeded with the 512-byte capacity; repeated physical-read stability remains unqualified. Transient read IO/timeout/absence results permit at most three complete UID-guarded acquisitions, separated by 20 ms. BUSY does not consume that acquisition budget: the integration waits outside the PAL call, with at most 100 shared 20-ms backoffs across the job. Exhausting that wait budget preserves the most recent actual acquisition failure, or returns BUSY when none occurred. Each failed buffer is discarded; partial pages never reach the payment API. UID mismatch, unsupported type and insufficient capacity are terminal errors. Payment submission and physical writes are never automatically retried. Type B/ISO-DEP interoperability and actual merchant acceptance of NFC replies remain unqualified.

`H2_NFC_READ` identifies activation, UID, type, allocation and content phases. `H2_NFC_READ_FRAMES` reports frame counts, last page/result and capacity/required length. After a READ failure it also samples IRQ, error, FIFO level and control registers without consuming FIFO bytes; `status_rc=0` validates that entire snapshot. `H2_ALIPAY_NFC_READ` reports each complete acquisition attempt separately. Reads stop at the end of the first message and validate retained CRC bytes. Larger-than-512 input fails explicitly. `H2_NFC_WRITE` reports activation and completed write/readback. `H2_NFC_WRITE_SELECT` reports each pre-write activation attempt, its last phase/command, cascade level and reply bit count. After failure it samples status registers after command cleanup; only `status_rc=0` validates the snapshot. Neither log prints payload or UID. The page distinguishes acquisition/write failures from API processing and logs `nfc_attempted`/`ble_attempted`.

`H2_NFC_WRITE_DETAIL` identifies full-tag acquisition, CC permission/version, static locks, supported TLV layout, trailing TLVs, capacity, length invalidation, payload, publication and exact readback. It records only CC/lock/TLV metadata, tag capacity, NDEF offset, control-TLV count and page-write call count/last page. `metadata=1` requires a complete tag read; zero page-write calls prove rejection before the page writer was invoked. Page-write calls can fail during register setup and do not by themselves prove that an RF command was transmitted. These diagnostics leave permission checks and single-write behavior unchanged.

`H2_ALIPAY_NFC_BUSY` marks the first availability wait and any exhausted wait budget. `H2_ALIPAY_NFC_READ` counts actual acquisition results and includes the accumulated `busy_waits`; a later successful read can therefore demonstrate recovery from contention without another SDK invocation or write.

After a transmitted reply, the App queries the current payment context. Internal SUCCESS, local write/readback or transmission submission does not establish a debit. Timeout remains `Result unknown; check merchant`. See [the qualification plan](payment_test_plan.md).

### Call ownership and remaining qualification

The production call path is [the payment worker](app/src/h2_alipay_payment_page.c) → Runtime logical-to-physical mapping → [public NFC PAL](../../../../pal/include/hal/h2_pal_nfc.h) → board arbitration → [the native provider](../../../../native_component_src/esp-idf6.x/h2_nfc_fm175xx/src/h2_esp_nfc_fm175xx.c) → [the portable FM175xx driver](../../../../libs/drivers/nfc/fm175xx/include/h2_fm175xx.h). Integration does not call the device driver directly. The client receives a complete NDEF message; its reply travels through the payment bridge, the entry-selected encoder and the same public PAL writer.

The board flag serializes each scan, read or write call. Runtime discovery continues between these calls; a discovery event does not reserve the reader for the complete read/process/write interaction. Both content acquisition and writeback reselect the expected UID. BUSY backoff therefore qualifies availability recovery only. A scan-free transaction or retention of an active selection would require an explicit public ownership contract before implementation; bypassing Runtime acquisition or adding a private board lock to the App would break that boundary.

The current writer reads the full CC-declared tag before checking permissions, lock/control TLVs and the replacement layout. This differs from message acquisition, which stops after the first complete NDEF. Terminal qualification must cover both reactivation and this additional pre-read. A failure in `activate` or `read_tag` with no page-write calls cannot be attributed to the reply's record encoding, while completed write/readback still requires separate merchant-format and payment-result validation. Repeated read stability and NFC write/readback remain unresolved; host tests and successful BUSY recovery do not qualify the entire payment flow.

## Optional BLE interaction

The shared integration still supports an explicitly selected BLE response mode for separate qualification. It converts canonical service UUIDs to PAL form, gates non-quick advertising and the final Notify fragment behind confirmation, and rejects duplicate submissions. Its 3802/4A02 service copies bounded writes to a worker queue and feeds the payment receive entry. Callbacks execute no business operation.

Connected/direct BLE results require current-attempt transmission and validated result data. Terminal outcome, Back, timeout and App exit stop acquisition and join workers before releasing resources. Failed cleanup retains handles for retry. Host synthetic tests do not establish real issuer or terminal acceptance.

## Validation

Run `//libs/clients/alipay:all`, the App/diagnostics/payment/client-adapter host tests, `tools:collector_test` and the final entry's identity, protocol, configuration and storage tests with the host configuration. NFC-only tests require zero payment BLE starts/scans/advertisements and cover write failure without fallback. Encoder tests exercise exact bytes, short/long NDEF lengths and the 512-byte boundary.

The library's seven fixed compatibility fixtures preserve previously cross-validated synthetic input/output samples; they no longer execute or import an external implementation. Physical qualification requires correct UID/package/Stage guards, actual H2Loader installation, manual phone authorization and scope-appropriate merchant/ledger evidence. A successful firmware build is not payment acceptance.
