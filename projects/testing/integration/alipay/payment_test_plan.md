# H106 Alipay payment qualification

This plan covers the current H106 entry: displayed payer QR and screen-on NFC response inside the Alipay wallet, using the owned compatible client and `PAYCODE | NFCPAY | SQUARE` (`0x4005`). Follow the wallet flow on PDF pages 12–17 and the current six-field callback header. Ordinary awake payment outside Alipay and screen-off payment are outside this scope. The integration App acquires no payment BLE radio. BLE compatibility in the client library has separate host replay coverage and is not a hardware acceptance claim for this entry. NFC payment has not passed real-device validation; the checks below remain qualification work.

## Prerequisites and evidence

Use an authorized account and merchant terminal, the unchanged factory SN/IMEI and a valid calibrated clock. Record the exact commit, dirty state, native toolchain/SDK, firmware/package hashes, board/device identity, terminal model/tag type and enabled feature profile. A successful binding query must be followed by a matching local snapshot of TID, UID, shared key, seed and completion record. Never reset or replace an unverified account implicitly.

Default session storage proves same-boot readback only. To qualify durability, explicitly build with the persistent provider and separately verify restart behavior. Retain legacy cleanup failures and prove that factory identity, certificates and unrelated namespaces remain intact. Do not reuse a successful host run as native or device evidence.

The entry prepares network IP and calibrated UTC without an extra HEAD gateway probe. Offline payer-code qualification must start from valid stored credentials/clock and explicitly remove device business routes. Merchant connectivity is an independent input; result queries still require device networking.

## Displayed payer QR

- Generate the documented 18/19 numeric bytes through the owned compatible API. Display the original payload using QR level Q, integer scaling and a quiet zone; qualify optical scans on the physical display. Code128 presentation is outside this entry.
- Exercise refresh, 60-second rotation and material clock changes. Retain each displayed attempt's original code for subsequent result queries, including a result for an earlier code after refresh.
- Test pending, transport failure, rejected, disabled, unbound and successful results. Show a paid receipt only after actual successful result evidence and confirm it against the merchant ledger. Generation, render/present and SDK callback success are insufficient.
- Verify previous/next/activate/back through Runtime Button release events and zero Touch PAL acquisition. Cancel/exit must join the worker and wipe private code/QR storage before dependencies are released.

## NFC wallet response

- Register NFC send and processing-status callbacks when entering the wallet's tap page, before reading a terminal. Use `alipay_nfc_recv_data_handle`, whose context is wallet `0`; ordinary `AWAKE` is `1`. Pass the complete NDEF and its exact length without extracting records, stripping headers or stopping at embedded zero bytes.
- Alipay confirmed Text-type NDEF replies on November 18, 2026, as reported by the user. Retain the current encoder's UTF-8 Text record with language `en`, with the complete message bounded to 512 bytes. Verify exact reply bytes, payload offsets and lengths on the terminal; the confirmed record type does not replace physical submission or payment qualification.
- Keep the same terminal close enough for discovery, complete input acquisition, SDK processing and one physical reply write. Check the exact discovered UID at each data operation; replacing the card must abort before writes to a different UID.
- Qualify first-message extraction, retained CRC checks, tag extent, static/dynamic locks, control TLVs, partial-page preservation and publish-last length handling. Unsupported tags must be rejected before sending Type 2 page commands.
- Exercise bounded transient activation/read retries and BUSY waits. BUSY before device access may wait at most 100 times for 20 ms with the same UID, encoded message and SDK invocation. Verify one encoding and at most one accepted physical write. A failed or ambiguous physical write must produce an explicit failure and zero payment queries, without an automatic second write, account reset, alternate response or payment BLE fallback.
- Complete the physical write without adding an automatic NDEF readback/equality gate. Input and writer preimage/expected fingerprints may describe existing buffers without extra NFC I/O; they do not prove merchant acceptance.
- After a completed reply, keep the attempt pending and use `alipay_query_last_nfc_pay_result` for the original NFC attempt. The current callback header defines notify status as internal SDK processing, superseding the PDF success-page example. Cover processing success with a write failure, pending/query failure after a successful write and an actual successful result including a zero amount. Verify any claimed paid state against the merchant ledger.
- Verify no payment BLE scan, advertising or GATT acquisition, quick-pay setting/guide flow, debug-token generation or online passive-tag scan/confirm fallback. H2Loader management transport remains separately owned.
- Cover timeout, back/exit during an operation, provider failure and repeated entry. Preserve borrowed dependencies after failed join/stop; release them only after successful cleanup.

## Status and close conditions

Focused host checks cover complete NDEF forwarding, callback registration before receive, one reply encoding, BUSY exhaustion, I/O/timeout write failures without retry/query, processing success without a paid receipt, and the existing SDK result query's failure/pending/paid states. Run the exact owning targets:

```sh
bazel test --config=macos_arm64 //projects/testing/integration/alipay/app:payment_page_test //projects/testing/integration/alipay/app:public_api_cpp_test //projects/testing/integration/alipay/app:payment_test //projects/testing/integration/alipay/app:nfc_test
```

Immutable replay preserves the original client fixtures and explicitly reports unsupported image/script exclusions. Those results do not qualify physical NFC interoperability.

Real-device NFC payment remains unpassed. Keep the issue open and do not create a PR until the requested device follow-up establishes the terminal profile and records actual merchant/result evidence. Preserve failed attempts and raw non-payload diagnostics alongside later successful retries. A fresh native package build is also required after the boundary cleanup; a missing toolchain is a blocked check, not a pass.

Source contracts: [client API](../../../../libs/clients/alipay/include/h2_alipay_api.h), [App binding API](app/include/h2_alipay.h), [App payment API](app/include/h2_alipay_payment.h), [reply encoder](app/include/h2_alipay_nfc.h).
