# Injected JavaScript evaluation

[h2_javascript.h](include/h2_javascript.h) evaluates a synchronous program in a fresh interpreter. The caller supplies Memory PAL, Time PAL and an optional native string bridge. The engine exposes no filesystem, network, module loader or task APIs, retains no input after returning and dispatches no asynchronous jobs.

Date reads an injected UTC snapshot and timezone operations use UTC. The upstream noncryptographic Math PRNG is seeded from that same snapshot. Cryptographic native operations belong to the caller's reusable crypto library. Memory, stack and interpreter-interrupt limits are explicit configuration; native callback work remains the caller's responsibility.

The immutable QuickJS input and package-local portability patch are declared under `third_party`. The patch adds per-runtime time injection, disables OS-dependent atomics, bounds floating-point integer conversion and fixes observed allocation-failure paths in context setup and bytecode cleanup. These fixes preserve successful JavaScript behavior and permit the wrapper to release every allocation after a failed initialization or compile.

Run `:javascript_test` for date, regular-expression, JSON and array behavior; native results and failures; capacity preservation; interrupt and heap limits; and every allocation-failure point observed by a simple evaluation. The same fault sequence runs under ASan/UBSan. Upstream source uses its existing low-level arithmetic conventions with UBSan disabled for that dependency; first-party code retains UBSan and the entire executable retains ASan.
