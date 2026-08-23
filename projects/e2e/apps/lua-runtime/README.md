# Lua Runtime E2E

The portable registry emits these nine fixed case IDs on Desktop, Browser, and
AMOLED:

- `vm-source-load`
- `coroutine-api`
- `coroutine-concurrency`
- `timer-wakeup`
- `component-lookup`
- `component-event`
- `cancel-timeout-race`
- `multi-vm-concurrency`
- `shutdown-with-waiters`

The App is the sole Runtime event-queue consumer. It copies a selected event to
one Lua job through `h2_lua_dispatch_runtime_event()`; the Lua Host never polls
the Runtime event queue independently.

`scheduler=cooperative` means multiple jobs made bounded progress; it does not
claim that one Lua VM or its coroutines executed in parallel on multiple CPUs.
