# Alipay integration diagnostics

This package owns the bounded [Trace contract](include/h2_alipay_trace.h), hashing, capture limits, sequence numbers and explicit drop/truncation statistics. It performs no IO and has one serialized producer; drain/reset only after its worker joins.

The [App API](../app/include/h2_alipay.h) owns binding/payment contracts. The concrete adapter owns initialization and compatible C callback adaptation. Trace has no dormant vendor-log, clock or allocator sampling classes; it records the operations emitted by the current adapter. Final-entry and host collector code own transport and serialization of evidence.
