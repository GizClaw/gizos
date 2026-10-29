# KCP allocation failure handling

`src/ikcp.c` replaces the source from the immutable KCP archive selected in `MODULE.bazel`. The source keeps the upstream license and public ABI. `overlay_files` applies it to both native and Host builds; the upstream header remains unchanged.

The replacement makes `ikcp_input()` return `-4` when receive-segment or ACK-list allocation fails. It leaves previously queued data and ACKs intact, releases a newly allocated segment if its ACK cannot be recorded, and never acknowledges newly received data that could not be retained. Existing `ikcp_send()` allocation-error returns also remain reachable when assertions are enabled.

The BLE iKCP tests inject failures into receive, ACK growth and send allocations, retry after pressure clears, and check stream error propagation and cleanup. When updating the archive pin, compare this replacement against the new upstream source before retaining or removing it.
