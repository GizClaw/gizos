# Log App

This portable Example verifies only the Runtime Log path. Its blocking entry borrows an initialized Runtime, emits one INFO record with scope `log` and message `Hello World`, then returns the Log provider result.

The App does not initialize a target UART, drain a target-owned buffer, loop, sleep, allocate memory, create a task, or depend on libco. Target launchers own Runtime assembly, Log transport initialization, draining, and the process or firmware lifecycle.

A BK3633 diagnostic launcher emits a direct UART-ready marker before GPIO and Runtime initialization, then repeatedly invokes the portable App and drains each Runtime Log record:

```text
H2_BK3633_LOG_UART_READY
[I][log] Hello World
```

The first line proves only that the launcher reached UART initialization. Each repeated second line is a portable Runtime Log result. Repetition belongs to the target launcher so a monitor attached after boot can still observe the PAL Log path; the portable App remains a single-call contract.
