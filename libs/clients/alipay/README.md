# Shared Alipay contracts

This package owns the provider-neutral [Alipay API](include/h2_alipay.h), initialization-once helper and bounded [trace contract](include/h2_alipay_trace.h). It contains no vendor headers, SDK bridge, business RPC implementation, physical identity acquisition or rendering.

The [vendor SDK adapter](../alipay_sdk/README.md) and [independent client](../alipay_independent/README.md) are separate packages. Their public C header and symbol names remain stable. The standalone integration App consumes the shared API and displays whichever provider the final artifact entry injects.

Keep board identity, provider selection, runtime assembly and display operations in their target or App owners. General cryptography belongs to the reusable crypto packages.
