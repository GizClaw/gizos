# Log App

This portable Example verifies only the Runtime Log path. Its blocking entry borrows an initialized Runtime, emits one INFO record with scope `log` and message `Hello World`, then returns the Log provider result.

The App does not initialize a target UART, drain a target-owned buffer, loop, sleep, allocate memory, create a task. Target launchers own Runtime assembly, Log transport initialization, draining, and the process or firmware lifecycle.
