# H106 Alipay payment qualification

This plan covers the current H106 entry: displayed payer QR and NFC wallet response, using the owned compatible client and `PAYCODE | NFCPAY | SQUARE` (`0x4005`). The integration App acquires no payment BLE radio. BLE compatibility in the client library has separate host replay coverage and is not a hardware acceptance claim for this entry. NFC payment has not passed real-device validation; the checks below remain qualification work.

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

- Identify the merchant terminal's real tag technology, NDEF record type and reply contract before accepting the current Type 2/Text profile. The current encoder uses one UTF-8 Text record with language `en`, with the complete message bounded to 512 bytes. The callback ABI alone does not prove that this profile matches the merchant.
- Keep the same terminal close enough for discovery, complete input acquisition, SDK processing and physical write/readback. Check the exact discovered UID at each data operation; replacing the card must abort before writes to a different UID.
- Qualify first-message extraction, retained CRC checks, tag extent, static/dynamic locks, control TLVs, partial-page preservation and publish-last length handling. Unsupported tags must be rejected before sending Type 2 page commands.
- Exercise bounded transient activation/read retries and BUSY waits. A failed or ambiguous physical write must produce an explicit failure, without an automatic second write, account reset, alternate response or payment BLE fallback.
- After a completed reply, query the original NFC attempt. Distinguish SDK processing status, completed physical write/readback and a financial result. Verify any claimed paid state against the merchant ledger.
- Verify no payment BLE scan, advertising or GATT acquisition. H2Loader management transport remains separately owned.
- Cover timeout, back/exit during an operation, provider failure and repeated entry. Preserve borrowed dependencies after failed join/stop; release them only after successful cleanup.

## Status and close conditions

Host checks cover the owning implementation, adapter, Runtime/UI and failure paths. Immutable replay preserves the original client fixtures and explicitly reports unsupported image/script exclusions. Those results do not qualify physical NFC interoperability.

Real-device NFC payment remains unpassed. Keep the issue open and do not create a PR until the requested device follow-up establishes the terminal profile and records actual merchant/result evidence. Preserve failed attempts and raw non-payload diagnostics alongside later successful retries. A fresh native package build is also required after the boundary cleanup; a missing toolchain is a blocked check, not a pass.

Source contracts: [client API](../../../../libs/clients/alipay/include/h2_alipay_api.h), [App binding API](app/include/h2_alipay.h), [App payment API](app/include/h2_alipay_payment.h), [reply encoder](payment/include/h2_alipay_nfc.h).
