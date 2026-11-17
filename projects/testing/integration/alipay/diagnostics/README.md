# Alipay integration diagnostics

This integration-owned package defines the test App's provider interface, initialization-once helper and bounded [trace contract](include/h2_alipay_trace.h). These are test orchestration and evidence contracts, not the production Alipay business API.

The [self-implemented client](../../../../../libs/clients/alipay/README.md) owns the business API. The App-facing interface here supports controlled fixtures and non-secret evidence without an external implementation.

Keep board identity, provider selection, runtime assembly and display operations in their target or App owners. General cryptography belongs to the reusable crypto packages.
