# KCP allocation failure handling

`ikcp_allocation_failures.patch` is a focused diff against KCP commit `b1a7a2101dcbb96017681a500d6b82bbe5a88766`, the immutable archive selected in `MODULE.bazel`. Bazel `patches` applies it to both native and Host builds. It preserves the upstream license, header and public ABI; no full upstream source copy is maintained here.

This is allocation-failure handling for GizOS's bounded embedded allocator, not a change to the KCP wire protocol or evidence that KCP fails under normal allocation. In the pinned upstream source, ACK-list allocation failure calls `abort()`, receive-segment allocation is dereferenced without a null check, and send allocation asserts before its existing null check. Configuring a bounded receive window and an appropriate allocator remains the adapter's responsibility.

The patch makes `ikcp_input()` return `-4` when receive-segment or ACK-list allocation fails. It leaves previously queued data and ACKs intact, releases a newly allocated segment if its ACK cannot be recorded, and never acknowledges newly received data that could not be retained. Existing `ikcp_send()` allocation-error returns also remain reachable when assertions are enabled.

The BLE iKCP tests inject failures into receive, ACK growth and send allocations, retry after pressure clears, and check stream error propagation and cleanup. When updating the archive pin, review each patch hunk against the new upstream source before retaining or removing it.
