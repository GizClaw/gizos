# PAL Core E2E App

This independent App checks all **46 operations** in the nine PAL Core vtables. Contract v2 contains **41 required cases**. A qualification run must execute all 41 with no FAIL, BLOCKED or NOT_RUN and restore its resource baseline. The mixed PAL App has been retired; [the Apps README](../README.md) records its replacements and remaining coverage gaps. This App owns the `pal-core/e2e/*` task namespace.

## Interface coverage

[api_coverage.json](app/api_coverage.json) maps each operation to its public header and normalized signature, owner case, additional cases, public wrapper and source-function evidence. [The registry](app/include/h2_pal_core_cases.inc) is the single source of stable IDs and the public case count.

| Capability | Operations | Contract-v2 evidence |
| --- | ---: | --- |
| Memory | 3 | alloc/realloc/free, NULL API behavior, alignment, preserved data, independent objects, 100 allocation/reallocation cycles and accounting |
| Log | 1 | Actual output for all four levels, exact scope/body and maximum message, invalid argument |
| Time | 6 | ms/us monotonic domain against an independent observer, sleep, wall validity/read/set/restore and monotonic independence |
| Timer | 7 | create/destroy/start/stop/reset/set_period/is_running; one-shot/auto-start/repeat, quiet stop, independent timers and 100 create/fire/destroy cycles |
| Task | 2 | Normal lifecycle plus the eight additional lifecycle, concurrency, stack and real allocation-failure scenarios below |
| Queue | 7 | create/destroy/send/send_latest/recv/reset/close; FIFO, copied payload, finite waits, full/empty, declared latest behavior, blocked writer close/reset and buffered close |
| Sync | 14 | All mutex/semaphore/condition operations; recursion, competing try_lock, eight-worker contention, multiple semaphore waiters, broadcast and 20 waiter generations |
| System Event | 5 | init/deinit/post/subscribe/unsubscribe; payload and type filtering, lifecycle, external unsubscribe during an active callback and self-unsubscribe |
| Firmware Info | 1 | Read actual image metadata twice and compare with the launcher's embedded build version |
| **Total** | **46** | Static completeness and actual provider execution are checked separately |

`interface_coverage_test` parses the public headers independently of the manifest. It rejects missing/duplicate operations, signature drift, unknown or duplicate case IDs, wrong registry wiring, wrong wrappers and absent source calls. Its own mismatch regressions guard against an accidentally weakened checker. This is a static completeness check, not a line/branch coverage metric; the real Desktop run must independently pass every registered case.

## Task acceptance

The single-task baseline is supplemented by:

- `task.once-context`: eight released workers, per-worker execution counts and unique result tokens, reverse joins; each entry must execute exactly once.
- `task.join-running`: join while a worker awaits a gate, with a separate real worker releasing it; success is checked against completed worker output.
- `task.join-completed`: wait for completed work before joining the task.
- `task.churn`: 100 create/run/join cycles, checking real resource counts after each cycle.
- `task.batch-churn`: 16 batches of eight tasks, alternating join order and checking every batch returns to its resource baseline.
- `task.stack-bounds`: request 4/16/64 KiB, independently observe the current worker's native pthread stack lower bound, and check the actual provider's stack-accounting storage remains owned until join.
- `task.start-failure`: deny a real Task provider stack allocation; require NO_MEMORY, no handle and no entry invocation, then restore allocation and successfully create/join another task.
- `task.partial-start`: allow two real starts and reject the third allocation; release/join all successful starts, check the resource baseline, then prove another task can start normally.

The host fault fixture only changes the allocator used by the real Task provider's existing stack-accounting configuration. It does not replace Task start/join with an always-success or fake implementation. Task names, context, nonzero `min_stack_size` and both start/join operations are exercised.

Desktop Task uses a provider-owned allocation as its actual pthread stack and retains it until join. The independent observer checks the current native stack size, and the provider regression checks a worker-local address lies inside the configured allocator's allocation. Platform-default size, requested minimum and configured policy floor are respected. This does not certify MCU affinity, priority, PSRAM placement or ISR behavior; those require the board launcher and real hardware. PAL Task itself has no suspend/cancel API.

## Execution and ownership

The App borrows a fully initialized Runtime and typed launcher observations. It never selects an OS/provider or reads process environment. Current cases have fixed finite workloads and operation budgets; launchers additionally need an independent process/device watchdog because Task join and mutex lock do not expose a timeout parameter.

All worker/callback state has a lifetime covering its actual use. Failed join, timer/sync destruction, queue close or wall restoration preserves the owning state and stops dependent execution. `h2_pal_core_e2e_cleanup()` retries it; recovering cleanup never upgrades the original failed case. System Event lifecycle tests own an exclusive fixture, not the Runtime's borrowed/default loop. Cross-thread subscription cleanup waits for callbacks before freeing context; self-unsubscribe stops admission without waiting for itself.

Every case emits a begin marker before it can block. Terminal records contain stable ID, PASS/FAIL/BLOCKED/NOT_RUN and PAL result. `complete` means every case has a terminal record; `qualified` requires every case PASS and successful cleanup. The final `resources.recovered` case compares real provider counts and forwarding allocator records against the snapshot taken before the App ran. Missing observation or fixture is BLOCKED, never an implicit PASS.

The `condition.bounded-wait` case allows spurious wakes and therefore does not by itself prove there are no incorrectly retained signals. The additional broadcast/generation cases exercise registered waiters, atomic mutex release and reacquisition, all-waiter delivery and reuse over 20 generations. Finite runs cover the stated contract and workloads rather than every possible scheduler interleaving.

## Real Desktop assembly

The host launcher initializes Runtime with real Desktop Memory, Log, Time, Timer, Task, Queue and Sync. Memory observation forwards to the actual Desktop allocator. Task-stack failure injection uses the same real allocator path. Resource snapshots come from object lifecycle counters maintained in the real provider: tasks, stack-accounting bytes, queues, mutexes, semaphores, conditions, timers and firmware metadata owners. Allocator observations add PAL-visible live allocation count and bytes; they do not claim to measure all C++ runtime caches or whole-process RSS.

Firmware Info is a reusable provider that copies the version embedded by the launcher's BUILD configuration. It is not an E2E stub. Its returned value is compared with the immutable image version. Timer uses a real steady clock and serial callback worker; no fake Timer is installed in the qualification run.

This launcher declares native macOS compatibility and uses the real Darwin event provider. The Runtime's unrelated event capability is bound to canonical unsupported, so the lifecycle test owns its fixture exclusively. Logs are checked against captured stderr. `CLOCK_MONOTONIC` and a 120-second OS watchdog provide independent timing observations. Wall calibration changes the Desktop provider's offset, not the OS clock; cleanup restores UTC. The public API does not restore calibration-source metadata.

## Commands

Run the complete real Desktop acceptance suite on this Mac:

```sh
bazel run --config=macos_arm64 --lockfile_mode=off \
  //projects/e2e/targets/cc_binary/pal-core:pal_core_e2e
```

Run the mandatory acceptance, inventory and controlled-failure regressions:

```sh
bazel test --config=macos_arm64 --lockfile_mode=off \
  //projects/e2e/targets/cc_binary/pal-core:host_regression_test \
  //projects/e2e/targets/cc_binary/pal-core:app_failure_test \
  //projects/e2e/targets/cc_binary/pal-core:async_event_test \
  //projects/e2e/apps/pal-core/app:interface_coverage_test
```

Keep Bazel disk cache enabled. `host_regression_test` now runs the same complete acceptance as the executable and requires a zero exit status; there is no exception for unsupported Timer/Firmware Info. A successful terminal result is:

```text
contract=2 passed=41 failed=0 blocked=0 not_run=0 complete=1 qualified=1 cleanup=0
```

`app_failure_test` separately validates the test harness with controlled faults: missing evidence, invalid configuration, Runtime queue proxy/fallback, deferred join, live timer callback after failed destruction, failed wall restoration and a queue close that returns success without waking its reader. These fake/forwarding regression results never count as platform capability passes. Provider-local tests additionally cover Timer callback stop/destroy, failed allocation, resource counters and metadata lifetime.

macOS is the execution environment for this change. The portable App can be reused by other launchers, but this strict host target does not yet declare Linux compatibility: its existing System Event provider needs the matching self-unsubscribe lifecycle fix and a full qualification run. No case is skipped to qualify macOS. An embedded package compiling likewise does not establish runtime acceptance there. Record the source revision, contract version, image/build metadata, provider configuration and full runtime report when qualifying any additional target.

## Web/WASM qualification

Run `bazel test //projects/e2e/targets/pkg_tar/pal-core:browser_test`. The same 41-case Core v2 contract passes in real Chromium with the pthread Web provider: 41 PASS, 0 FAIL/BLOCKED/NOT_RUN, qualified=1 and cleanup=0. The launcher uses the real Runtime and real PAL providers, with a forwarding Task-stack allocator for controlled allocation failures and actual provider resource counters. Wall-clock changes are platform-local offsets. Firmware Info comes from immutable launcher build metadata.

Additional mandatory checks verify that C main runs on a Worker, Wasm memory is a SharedArrayBuffer, and the page is cross-origin isolated. Two distinct PAL Task Workers must cross a shared atomic barrier without sleep, yield or browser proxy calls. Each requested stack lower bound is measured inside its native Worker. One hundred start/join cycles must leave the heap byte-for-byte unchanged. A Worker-side `pthread_join` returns before Emscripten frees the joined thread's control block (`struct pthread`, TLS and TSD; PAL owns the stack): the joiner posts `cleanupThread` to the browser main thread, and nothing orders that message against later proxied calls. Every probe join therefore waits until the runtime no longer tracks that pthread before the heap is sampled. A separate regression holds the browser main thread across one join, requires the control block to still be allocated when the join returns, and requires the drained heap to equal the baseline taken before the task existed. External unsubscribe must wait for the active handler to finish, and platform teardown must succeed. These assertions fail the browser test rather than being reported as optional diagnostics.

The artifact contains COOP/COEP `_headers`; use `:serve`, or configure the same response headers on another HTTPS/localhost host. This qualifies Core, not browser capabilities outside Core (for example WebCodecs or physical devices).

Browser integration uses one `h2_web_main_call(callback, context)` entry. A browser operation is one direct `EM_JS` callback, with no C dispatch/wrapper pair or per-operation context struct. Callers pass pointers to typed C argument values; the shared JS bridge reads them and returns the scalar result by value. It handles synchronous results and Promises, preserving doubles, pointers and 64-bit integers. There are no argument-count-specific bridge macros.

## DevKit ESP32-S3

The independent managed package is `//projects/e2e/targets/h2loader_tar_zlib/pal-core/devkit:package`. It runs the same complete Core contract on actual ESP providers, with native resource counters, real PSRAM stack observation/failure injection and a separate serial management allocator. See the [launcher guide](../../targets/h2loader_tar_zlib/pal-core/devkit/README.md) for build/install commands and device evidence. No missing capability or observation can qualify this target as PASS.

The 2026-09-28 DevKit run of `pal-core-devkit-20260928-r6` passed all 41 cases, with zero FAIL/BLOCKED/NOT_RUN and cleanup=0. The stored [device report](../../targets/h2loader_tar_zlib/pal-core/devkit/evidence/2026-09-28/qualified.json) also records the 100-cycle Task probe, actual 4/16/64 KiB stacks, allocation failure recovery and final H2Loader/image identity.

## BK7258 AP qualification

The managed package `//projects/e2e/targets/h2loader_tar_zlib/pal-core/bk7258_v3_202405:package` runs the complete Core v2 contract on real BK providers. The [board launcher](../../targets/h2loader_tar_zlib/pal-core/bk7258_v3_202405/README.md) records the UART1 install and two 41/41 runs of `pal-core-bk-20260928-r7`, including one after a normal App reboot. Neither run has FAIL, BLOCKED or NOT_RUN; cleanup and App confirmation both succeed.

The launcher independently verifies the real 4/16/64 KiB task stacks and injects failure into the actual stack allocation path. Native object and PAL allocation counters must match their initial baselines. Management transport and client metadata buffers use a separate SDK PSRAM allocator, so host status polling cannot affect these comparisons. No count is replaced with zero and no Core assertion is relaxed. Runtime's Atomic prerequisite has its own AP cross-core check; Atomic remains outside the PAL Core case count.
