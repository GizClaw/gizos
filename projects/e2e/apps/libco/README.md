# Libco Smoke App

This portable E2E App exercises one `h2_libco_t` executor with explicitly
sized stackful coroutine tasks. It covers FIFO yield, wait/wake, timeout,
cancellation, task-to-task and root join, bounded cleanup, and repeated context
switching.

The default coroutine stack is 8 KiB and the default stress phase performs
10,000 cooperative switches. At most three coroutine tasks are live at once.
Their stacks are separate heap allocations; only heap, globals, Runtime
services, and the surrounding address space are shared.

Target entries are provided by Desktop, Browser, ESP32-S3 DevKit, BK7258 V3,
and a BK3633 diagnostic launcher. Desktop, Browser, DevKit, and BK7258 use the 8 KiB default;
the constrained BK3633 diagnostic image explicitly selects 2 KiB per
coroutine while retaining the same phases and 10,000 switches. The portable
App does not create OS/RTOS tasks, select a CPU backend, confirm an H2Loader
image, or claim stack-overflow immunity.

The Browser entry runs the registry from the browser root because the registry
owns the executor under test. Its Emscripten Fiber backend allocates separate
C and Asyncify stacks and requires neither pthreads nor SharedArrayBuffer.

Successful execution emits each `H2_LIBCO_SMOKE_STAGE` marker followed by one
`H2_LIBCO_SMOKE_PASS rc=0` after executor destruction. The first failure emits
`H2_LIBCO_SMOKE_FAIL` and returns nonzero after bounded cleanup.

Set `H2_LIBCO_SMOKE_INJECT_FAILURE=1` for the Desktop executable to exercise
the FAILED status surface and nonzero exit path with an invalid portable config.
`H2_LIBCO_SMOKE_AUTO_CLOSE=1` skips the interactive wait after projecting the
terminal state so automated validation can verify display close and Runtime
deinitialization; normal execution remains visible until the user closes it.
