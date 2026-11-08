# Development constraints

Read before changing App, library, Runtime, or provider behavior. Use [structure](structure.md) to choose ownership; public headers are the API specification.

## Dependencies and public contracts

- Portable Apps depend on Runtime and portable public libraries, not board headers, SDK objects, backend internals, or launcher types. Portable libraries receive platform capabilities through injected PAL APIs, configuration, or callbacks; keep Runtime-aware adapters separate from independent cores.
- Public headers expose stable types and functions. Keep private state, worker handles, SDK types, and physical wiring behind the owning boundary. Update header documentation with contract changes rather than maintaining API copies in Guides.
- App entries receive an initialized `h2_runtime_t *` and optional App-owned public configuration. The artifact entry owns Runtime construction/destruction; Apps initialize only the library integrations they use.
- Define required logical `component_id` values in the App contract. The launcher must validate every required mapping before calling the App; missing mappings are wiring errors. Resolve a physical ID through Runtime only for the immediate PAL operation, never as persistent App state or a public contract.
- Use the injected Runtime/PAL capability contract, not board identity or target macros, to choose portable behavior. API-object presence does not prove a required operation is supported. Propagate missing required capabilities; optional fallback must be explicit.
- Third-party global ABI bridges belong to their library integration. Prefer explicit context injection; unavoidable singleton bridges need defined init/deinit and duplicate-init behavior and must not become global PAL proxies.

## State, effects, and execution context

- Keep App/business state under one main-loop writer. Runtime events provide ordered facts; Runtime state provides current snapshots, including initialization. Workers and provider callbacks return facts/results rather than mutating App state.
- LVGL subjects expose observable state; widgets consume bindings and props without storing a second business model. Observers perform bounded UI projection, not a second reducer or cyclic subject updates. Keep large payloads and job handles in private state.
- Represent each one-time operation as an explicit action/effect. Repeating the same request must preserve its count and order even when a state value is unchanged. Network, storage, decoding, and other blocking effects run outside event handlers and observers.
- All LVGL calls, subject mutation, and widget lifecycle stay on the App/LVGL thread. A target lock does not permit multiple business-state writers. Dispatch library completions on the caller/main-loop thread, never inline from workers, submit, cancel, or PAL callbacks.
- Runtime owns input acquisition and system/component producers. Consume its events/state rather than polling the same PAL inputs again, creating duplicate input tasks, or calling private Runtime producer APIs. Ordinary UI actions enter App transitions directly.
- Use `h2_runtime_wait_notify()` for Runtime wake or an App deadline; then drain Runtime events completely before waiting again and service library dispatch queues. Wake signals coalesce. Keep dispatch work bounded and arrange another wake if bounded work remains.
- App/library completions use public custom events containing copied values, or an owned bounded dispatch queue plus `h2_runtime_notify()`. Do not impersonate Runtime system/component facts. Handle queue saturation explicitly.
- An ISR may use only interfaces explicitly documented as ISR-safe. Defer blocking work, allocation, App transitions, and UI operations to task context; a nonblocking task API is not automatically ISR-safe. Keep callback work bounded and obey the provider's concurrency contract.

## Lifetime, results, and cancellation

- Make ownership and borrowing explicit for configurations, API objects, buffers, callback contexts, and results. Borrowed dependencies must outlive every operation and resource using them; copying a structure does not extend its pointed-to storage lifetime.
- For asynchronous results, define acceptance, completion context, and who releases the result. Use stable identity plus generation for owner-scoped completion; reject stale state updates while still releasing their resources. Never queue raw callbacks, task handles, or unverifiable object pointers as custom-event payloads.
- Define who cancels work when its owner exits. Cancellation is a request, not proof of completion: wait for the documented terminal outcome or join before freeing context. Prevent late completions from touching a replaced page, job, or Runtime.
- Shutdown stops new commands/producers, requests stop/cancel, wakes blocked users, joins workers, drains pending completion cleanup, then deinitializes resources in reverse ownership order. App tasks, timers, subscriptions, callbacks, and PAL borrows must end before App return or Runtime destruction.
- A failed join retains the task handle, entry context, and borrowed dependencies for retry. Do not free them or report successful teardown. Use Runtime's public destruction API when cleanup evidence is required.
- Delete bound widgets and remove observers before deinitializing subjects or freeing their string backing. Deinitialize library integrations while borrowed PAL APIs still live; integration cleanup must not destroy shared providers.

## Allocation, errors, and validation

- Honor the owner's allocator contract. Portable integrations use injected Memory PAL instead of SDK or process-global allocation; pair allocation/free through the same allocator, check size arithmetic and allocation failures, and preserve valid state after partial initialization.
- Give every acquired handle/buffer an owner and cleanup path, including failure paths. Close/wake concurrent users before destroying shared queues or synchronization resources; keep dependencies alive until all users return.
- Preserve public result semantics. Distinguish unsupported, timeout/empty, cancellation/closed, saturation, and operational failure as the relevant API defines them. Do not turn required-capability failures into success or silently lose accepted completion work.
- Portable firmware libraries use injected Log PAL for diagnostics, not implicit `stdout`, `stderr`, or `FILE *` dependencies. Keep platform policy and backend-specific recovery in their owners.
- Validate the changed behavior through the owning package and affected public boundary. Reuse production event/transition paths in App tests; do not create test-only shortcuts that bypass them. See the [Bazel guide](../tools/bazel/README.md) for build and test rules.

Consult the live [Runtime](../libs/runtime/include/h2_runtime.h), [event delivery](../libs/runtime/include/h2_runtime_event.h), [custom event](../libs/runtime/include/h2_runtime_custom_event.h), and [PAL](../pal/include/h2_pal.h) headers for exact API contracts.
