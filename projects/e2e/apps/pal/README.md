# PAL E2E App

This headless portable App runs provider-neutral PAL integration cases through
an initialized Runtime. The `core` suite validates Time, Timer, Task, Queue,
Mutex, Semaphore, Condition wait/signal, and blocking wake plus timeout wait-u32.
It does not assume a real filesystem rejects `mkdir`; that old unsupported
case was removed rather than treating a supported filesystem as a failure.
Its provider-neutral concurrency case
runs three producers and three consumers through a bounded queue, starts them
through a condition-variable barrier, contends on one mutex, and proves that
all 96 uniquely identified messages are produced and consumed exactly once
before every task is joined and every synchronization object is destroyed. The
result retains an opaque cleanup owner if a Task join cannot complete; the
launcher must keep the Runtime alive, make platform progress, and call
`h2_pal_e2e_cleanup()` until that owner is reclaimed before Runtime teardown. The
independent `mqtt` suite uses a launcher-provided broker endpoint, unique client
ID, topic, payload, timeout, and network buffer.

The App owns the MQTT connect, subscribe, publish echo, disconnect, deadline,
result, and cleanup flow. It does not read process environment variables,
generate host randomness, start a broker, select a concrete provider, or print
results. Desktop owns the public-broker configuration, unique token generation,
Runtime/provider assembly, loopback broker fixture, and process output.
The Preference suite round-trips every public type and a 16 KiB blob, reopens
or reboots twice, exercises same-key replacement, remove, clear and terminal
replay. Desktop drives it through the real SQLite provider; DevKit and Tiga
V4.2 package it as the H2Loader-managed `pal-pref` App. Preference is selected
alone because it returns cross-boot actions; core and MQTT can be combined.
Backend- and adapter-local PAL tests remain owned by their target component;
this App owns only reusable Runtime-level integration flows. Desktop can select
core and MQTT independently; Browser selects only core and runs it in one
platform-owned cooperative task.

The standalone `wifi` suite disconnects STA, queries STA and Netif, and rejects
stale connection/IP/default-route flags. A launcher must select it alone and
provide a non-Wi-Fi control link; it leaves STA disconnected and never changes
saved credentials. This currently
does **not** cover scan, connect, AP clients, reconnect, or Runtime Wi-Fi events.

The standalone `filesystem` suite (`H2_PAL_E2E_SUITE_FILESYSTEM`) runs only the
`HOST_FILESYSTEM` case against a writable `/data` mount: mkdir and stat `is_dir`,
write, stat exact size and `!is_dir`, reject mkdir on the regular file, read,
EOF read returning 0 bytes, backward seek to offset 3 and verify re-read bytes,
and cleanup.

The separate `pal_atomic_e2e` library runs two concurrent workers supplied by the launcher. The DevKit H2Loader package at `//projects/e2e/targets/h2loader_tar_zlib/pal-atomic/devkit:package` pins one task to each ESP32-S3 core. PAL allocates internal-RAM u32/i32/flag storage; both cores use standard C11 atomics. Each counter must reach 2,000,000 after 1,000,000 increments per core. Concurrent CAS, exchange, and flag cases must also be exact. A control counter allocated with `MALLOC_CAP_SPIRAM` uses the same standard `atomic_fetch_add` for 5,000,000 increments per core. Its `EXPECTED_LOSS lost=N` result records PSRAM lost updates; `LOSS_NOT_REPRODUCED` is a warning and does not fail the suite. Each case and summary emit structured UART lines.

A case whose worker join or timer destroy fails keeps its resources in
`retained_cleanup`; remaining suites (including MQTT) are skipped until
`h2_pal_e2e_cleanup()` succeeds.

The AC791N launcher is
`//projects/e2e/targets/h2loader_tar_zlib/pal/jieli_ac791n_devkit:package`.
It uses the shared H2Loader board layout and Bazel task policy, runs the
standalone Filesystem suite, then Core, then Wi-Fi, and prints per-case
`H2_PAL_E2E` results plus a repeating summary over UART1. It deliberately leaves
the diagnostic App unconfirmed so reset recovers to Loader; this is not an App
confirmation or full Loader lifecycle test.
