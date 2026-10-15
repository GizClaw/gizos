# WL82 core PAL SDK boundary

## Non-blocking synchronization

PAL timeout `0` means try without waiting. The SDK's `os_mutex_pend` and `os_sem_pend` instead interpret timeout `0` as an infinite wait. Therefore `h2_jieli_wl82_sdk_port.c` uses the public `os_mutex_accept` and `os_sem_accept` operations for this case; substituting `pend(..., 0)` would introduce a hang.

The pinned AC791N SDK revision is recorded in `tools/bazel/native_versions/jieli_ac791n_sdk_commit.txt`. At revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`:

- `include_lib/system/os/os_api.h:355` declares `int os_sem_accept(OS_SEM *sem)`.
- The same header at line 440 declares `int os_mutex_accept(OS_MUTEX *mutex)`.
- Both are documented as non-blocking, returning `0` on success and `OS_TIMEOUT` if acquisition fails. Neither may be called from an ISR or a critical section.
- `cpu/wl82/liba/system.a`, member `os_api.c.o`, exports both as defined text symbols, not unresolved references.

### Reproduce the export check

From the SDK checkout matching the pinned revision, use GNU `nm` (the SDK archive is not a macOS Mach-O library):

```sh
git rev-parse HEAD
rg -n 'int os_(sem|mutex)_accept' include_lib/system/os/os_api.h
nm -A cpu/wl82/liba/system.a | rg ' T os_(mutex|sem)_accept$'
```

The defined-symbol lines observed for this revision are:

```text
cpu/wl82/liba/system.a:os_api.c.o:00000000 T os_mutex_accept
cpu/wl82/liba/system.a:os_api.c.o:00000000 T os_sem_accept
```

The linker-script export guard and its regression test belong to the AC791N DevKit board integration (GizClaw/gizos PR #178) and are not part of this package.

When changing the SDK revision, repeat the export check above. The firmware link check belongs to the AC791N DevKit board integration (GizClaw/gizos PR #178), using its configured SDK/toolchain environment and package targets; it is not part of this package.

Export and link checks establish symbol availability only; they do not substitute for on-device synchronization and boot-lifecycle acceptance.

## Timer dispatch

The SDK's `sys_timer_add` / `sys_timeout_add` dispatch to the registering task,
not an arbitrary PAL worker. Timer PAL therefore synchronously marshals every
lifecycle operation to the SDK `sys_timer` service using
`sys_timeout_add_to_task`; callbacks and delayed reclamation run on that same
service. Calls from a timer callback execute inline to avoid self-deadlock.
Interrupt and pre-scheduler calls are rejected. Application code must keep
timer callbacks short and non-blocking, as required by the public PAL contract.

The request borrows caller pointers until service completion; it must not time
out and leave a queued request referring to expired stack memory. Failure to
enqueue leaves the timer untouched and retryable. Host behavior tests cover
cross-caller operations, callback destruction, stale fires and enqueue failure;
the AC791N public PAL E2E launcher is required to establish real SDK dispatch.

## System event dispatch

At pinned SDK revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, `cpu/wl82/liba/event.a` contains LLVM bitcode (`event.c.o`, magic `BC C0 DE`). The following dispatch facts come from its IR and `include_lib/utils/event/event.h`; the public `u8 len` permits 255 bytes, but the dispatcher's actual read buffer holds only 32 bytes with no bounds check, so PAL always sends a 32-byte envelope.

`sys_event_notify` lazily allocates a 292-byte pool: a 36-byte cbuf header and a 256-byte ring shared with SDK events. It copies a 4-byte event header plus the payload under local interrupt masking and a spinlock, posts the event semaphore, and never waits for delivery. A full ring drops the message and returns -12. Notify is interrupt-safe once the pool exists because `os_sem_post` uses `xQueueGiveFromISR` in interrupt context; PAL nevertheless rejects interrupt posts because registry locking, generation operations and heap copies are not interrupt-safe.

The SDK `sys_event` task dispatches matching events by posting a `Q_CALLBACK` to the registering task, recorded as the handler's owner. That task executes the callback inline inside `os_taskq_pend`; handlers do not run on `sys_event`. The SDK waits for completion, retrying a full owner queue after two ticks, with a default 40000 ms timeout that causes an assertion or `P33_SYSTEM_RESET()`. PAL handlers must remain short and non-blocking. SDK unregister only marks a handler deleted and cannot recall an already-posted owner callback.

The port therefore owns a persistent `h2_sysevt` task (priority 20, 1024 stack words, 32 queue words), registers `(0x0100, 0x50, 0)` there, signals readiness, and loops in `os_taskq_pend`. First start waits at most 1000 ms; later starts reuse the task. The trampoline accepts only matching type/from and exactly 32 bytes. The SDK's `app_core` all-events handler also sees these events and ignores them. Neither the owner task nor its registration is removed on PAL deinit.

The envelope carries a 64-bit generation ceiling, timestamp, source ID, type, flags, payload size and an 8-byte inline/pointer union. Payloads of at most 8 bytes are copied inline; 9..1024 bytes use an owned heap copy released after delivery or discard. Larger payloads return INVALID_ARG and copy allocation failure returns NO_MEMORY. Callbacks borrow the reconstructed event and payload only for their duration. Post returns enqueue status rather than callback return values; its timeout only bounds registry lock acquisition.

PAL reserves at most four in-flight events, occupying at most 144 bytes of the shared SDK ring including headers. Either PAL depth exhaustion or SDK -12 returns FULL and increments the image-lifetime overflow counter; other SDK post failures return IO. Each queued event retains an operation reference and per-subscription queued counts. Generation snapshots exclude subscriptions created after admission. External unsubscribe retires admission and waits for queued/running callbacks, including during CLOSING; any unsubscribe on the dispatcher task returns without waiting, and retiring queued callbacks are skipped. Last-owner deinit closes admission but keeps queued delivery alive until the final reference releases the registry and lock.

### Reproduce the system event export check

From the pinned SDK checkout, the observed GNU `nm` export check is:

```sh
nm -A cpu/wl82/liba/event.a | rg ' T (sys_event_notify|register_sys_event_handler)$'
```

```text
cpu/wl82/liba/event.a:event.c.o:-------- T register_sys_event_handler
cpu/wl82/liba/event.a:event.c.o:-------- T sys_event_notify
```

The host fake, pthread driver and `//tools/bazel:jieli_sys_event_port_test` cover queueing, ownership, quiescence and the SDK port contract. These checks establish software behavior only; native linking and the AC791N board acceptance round remain separate validation.

## Caller allocator ownership

Mutex, semaphore, condition and queue configs honor `config->allocator`.
The creating allocator stays with each PAL object until destruction or failed
creation cleanup. Queue backing storage and its internal PAL mutex/conditions
use the same allocator. A NULL allocator retains the SDK default allocation
path. SDK-native mutex/semaphore storage remains paired with the SDK's own
create/destroy functions, as with ESP's native semaphore handles. Task options
and timer configs have no caller-allocator field; no new API field is added.

The pinned `os_api.c.o` and `queue.c.o` IR establish the SDK failure boundary:
mutex/semaphore deletion calls `vQueueDelete` and returns zero. With valid
quiescent objects there is no recoverable returned delete failure to retain.
`xQueueGiveMutexRecursive` returns failure only when the current native task
is not the owner; `os_mutex_post` also rejects IRQ/critical-section calls.
For a valid task-context PAL owner, the native mutex was acquired once and PAL
recursion is handled above it, so final native unlock has a matching owner.
No retry or ownership mode is introduced for corruption or invalid-context
calls. Condition relock remains mandatory; the internal helper reports IO and
records lack of ownership if an SDK failure prevents it, avoiding a second
invalid unlock. This is not a promise to recover invalid native objects.
