# Alipay integration diagnostics

This integration-owned package defines the test App's provider interface, initialization-once helper and bounded [trace contract](include/h2_alipay_trace.h). These are test orchestration and evidence contracts, not the production Alipay business API.

The [vendor SDK adapter](../vendor_sdk/README.md) and self-implemented client are selected at build time. Both use the original Alipay public API declarations; the App-facing interface here keeps controlled tests and non-secret evidence independent of the backend.

Keep board identity, provider selection, runtime assembly and display operations in their target or App owners. General cryptography belongs to the reusable crypto packages.
